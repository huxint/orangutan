#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orangutan {

using json = nlohmann::json;

enum class HookEvent : std::uint8_t {
    before_tool_call,
    after_tool_call,
    message_received,
    message_sending,
    session_start,
    session_end,
};

struct HookEventHash {
    std::size_t operator()(HookEvent e) const noexcept {
        return std::hash<std::uint8_t>{}(std::to_underlying(e));
    }
};

struct HookDef {
    std::string path;
    HookEvent event;
    std::string filename;
};

struct HookResult {
    int exit_code = 0;
    std::string stderr_output;
    bool timed_out = false;
};

struct DispatchResult {
    bool allowed = true;
    std::string blocked_by;
    std::string block_reason;
};

class HookManager {
public:
    void load_from_directories(const std::vector<std::string> &directories);

    // Dispatch hooks for an event. Returns DispatchResult with rejection details.
    // For before_tool_call: returns allowed=false if any hook blocked (non-zero exit).
    [[nodiscard]]
    DispatchResult dispatch(HookEvent event, const json &context) const;

    [[nodiscard]]
    size_t hook_count(HookEvent event) const;

    [[nodiscard]]
    size_t total_hooks() const;

private:
    std::unordered_map<HookEvent, std::vector<HookDef>, HookEventHash> hooks_;
};

// Convert event enum to directory name string
std::string hook_event_to_string(HookEvent event);

// Build JSON context for before/after tool call hooks
json build_before_tool_call_context(std::string_view tool_name, const json &tool_input);
json build_after_tool_call_context(std::string_view tool_name, const json &tool_input, std::string_view tool_result, bool is_error);

// Build JSON context for message hooks
json build_message_context(HookEvent event, std::string_view role, std::string_view content);

// Build JSON context for session hooks
json build_session_context(HookEvent event, std::string_view session_id, size_t message_count = 0);

void dispatch_session_start(HookManager *hook_manager, std::string_view session_id, size_t message_count = 0);
void dispatch_session_end(HookManager *hook_manager, std::string_view session_id, size_t message_count = 0);

} // namespace orangutan
