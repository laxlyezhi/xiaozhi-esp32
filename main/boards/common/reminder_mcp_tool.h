#ifndef REMINDER_MCP_TOOL_H
#define REMINDER_MCP_TOOL_H

#include "mcp_server.h"
#include <esp_timer.h>
#include <vector>
#include <string>

class ReminderMcpTool {
public:
    ReminderMcpTool();
    ~ReminderMcpTool();

    void Initialize();

    void FireReminder(int id);

private:
    struct Reminder {
        int id;
        std::string message;
        esp_timer_handle_t timer;
        bool active;
        int64_t trigger_time;
    };

    struct TimerArg {
        ReminderMcpTool* tool;
        int id;
    };

    static constexpr int kMaxReminders = 8;
    std::vector<Reminder> reminders_;
    std::vector<std::unique_ptr<TimerArg>> timer_args_;
    int next_id_ = 1;

    ReturnValue HandleSetReminder(const PropertyList& properties);
    ReturnValue HandleListReminders(const PropertyList& properties);
    ReturnValue HandleCancelReminder(const PropertyList& properties);

    static void ReminderTimerCallback(void* arg);

    cJSON* BuildReminderListJson();
};

#endif