#include "reminder_mcp_tool.h"
#include "application.h"
#include "board.h"
#include "display/display.h"
#include "assets/lang_config.h"
#include <esp_log.h>

static const char* TAG = "ReminderMcpTool";

ReminderMcpTool::ReminderMcpTool() {
}

ReminderMcpTool::~ReminderMcpTool() {
    for (auto& r : reminders_) {
        if (r.timer) {
            esp_timer_stop(r.timer);
            esp_timer_delete(r.timer);
        }
    }
}

void ReminderMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool("self.reminder.set",
        "Set a reminder. The device will alert the user with a sound and notification when the time is up.\n"
        "Args:\n"
        "  `message`: The reminder message to show when the time is up.\n"
        "  `seconds`: The number of seconds from now to trigger the reminder (minimum 1, maximum 86400).\n"
        "Returns: The reminder id and info.",
        PropertyList({
            Property("message", kPropertyTypeString),
            Property("seconds", kPropertyTypeInteger, 1, 86400)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return HandleSetReminder(properties);
        });

    mcp_server.AddTool("self.reminder.list",
        "List all active reminders.\n"
        "Returns: A JSON array of active reminders with their id, message, and remaining seconds.",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            return HandleListReminders(properties);
        });

    mcp_server.AddTool("self.reminder.cancel",
        "Cancel an active reminder by its id.\n"
        "Args:\n"
        "  `id`: The reminder id to cancel.\n"
        "Returns: true if cancelled, false if not found.",
        PropertyList({
            Property("id", kPropertyTypeInteger)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return HandleCancelReminder(properties);
        });

    ESP_LOGI(TAG, "ReminderMcpTool initialized");
}

ReturnValue ReminderMcpTool::HandleSetReminder(const PropertyList& properties) {
    auto message = properties["message"].value<std::string>();
    auto seconds = properties["seconds"].value<int>();

    if (seconds < 1 || seconds > 86400) {
        throw std::runtime_error("Seconds must be between 1 and 86400");
    }

    if (reminders_.size() >= kMaxReminders) {
        auto it = std::find_if(reminders_.begin(), reminders_.end(),
            [](const Reminder& r) { return !r.active; });
        if (it != reminders_.end()) {
            esp_timer_stop(it->timer);
            esp_timer_delete(it->timer);
            reminders_.erase(it);
        } else {
            throw std::runtime_error("Maximum number of reminders reached");
        }
    }

    int id = next_id_++;

    auto arg = std::make_unique<TimerArg>();
    arg->tool = this;
    arg->id = id;
    TimerArg* arg_ptr = arg.get();
    timer_args_.push_back(std::move(arg));

    esp_timer_handle_t timer;
    esp_timer_create_args_t timer_args = {
        .callback = ReminderTimerCallback,
        .arg = arg_ptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "reminder",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, seconds * 1000000LL));

    int64_t trigger_time = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    reminders_.push_back({id, message, timer, true, trigger_time});

    ESP_LOGI(TAG, "Reminder set: id=%d, message='%s', seconds=%d", id, message.c_str(), seconds);

    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "id", id);
    cJSON_AddStringToObject(json, "message", message.c_str());
    cJSON_AddNumberToObject(json, "seconds", seconds);
    return json;
}

ReturnValue ReminderMcpTool::HandleListReminders(const PropertyList& properties) {
    return BuildReminderListJson();
}

ReturnValue ReminderMcpTool::HandleCancelReminder(const PropertyList& properties) {
    auto id = properties["id"].value<int>();

    auto it = std::find_if(reminders_.begin(), reminders_.end(),
        [id](const Reminder& r) { return r.id == id && r.active; });

    if (it == reminders_.end()) {
        throw std::runtime_error("Reminder not found or already fired: " + std::to_string(id));
    }

    esp_timer_stop(it->timer);
    esp_timer_delete(it->timer);
    reminders_.erase(it);

    ESP_LOGI(TAG, "Reminder cancelled: id=%d", id);
    return true;
}

void ReminderMcpTool::FireReminder(int id) {
    auto it = std::find_if(reminders_.begin(), reminders_.end(),
        [id](const Reminder& r) { return r.id == id && r.active; });

    if (it == reminders_.end()) {
        return;
    }

    std::string message = it->message;
    it->active = false;

    esp_timer_delete(it->timer);
    reminders_.erase(it);

    ESP_LOGI(TAG, "Reminder fired: id=%d, message='%s'", id, message.c_str());

    Application::GetInstance().Schedule([message]() {
        auto* codec = Board::GetInstance().GetAudioCodec();
        int original_volume = codec->output_volume();
        codec->SetOutputVolume(100);
        Board::GetInstance().GetDisplay()->ShowNotification(message.c_str(), 10000);
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_REMINDER);
        codec->SetOutputVolume(original_volume);
    });
}

void ReminderMcpTool::ReminderTimerCallback(void* arg) {
    auto* timer_arg = static_cast<TimerArg*>(arg);
    if (timer_arg && timer_arg->tool) {
        timer_arg->tool->FireReminder(timer_arg->id);
    }
}

cJSON* ReminderMcpTool::BuildReminderListJson() {
    cJSON* json = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();

    for (const auto& r : reminders_) {
        if (!r.active) continue;

        int remaining_sec = 0;
        if (r.trigger_time > now) {
            remaining_sec = static_cast<int>((r.trigger_time - now) / 1000000LL);
        }

        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", r.id);
        cJSON_AddStringToObject(item, "message", r.message.c_str());
        cJSON_AddNumberToObject(item, "remaining_seconds", remaining_sec);
        cJSON_AddItemToArray(json, item);
    }

    return json;
}