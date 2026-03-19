#include "features/hooks/hook-manager.hpp"

#include "infra/subprocess/subprocess.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <string_view>

namespace orangutan {

// ── Event name mapping ──────────────────────────

using EventNameEntry = std::pair<HookEvent, std::string_view>;

constexpr std::array<EventNameEntry, 6> event_names = {{
    {HookEvent::before_tool_call, "before_tool_call"}, {HookEvent::after_tool_call, "after_tool_call"},
    {HookEvent::message_received, "message_received"}, {HookEvent::message_sending, "message_sending"},
    {HookEvent::session_start, "session_start"},       {HookEvent::session_end, "session_end"},
}};

std::string hook_event_to_string(HookEvent event) {
    if (const auto *const it = std::ranges::find(event_names, event, &EventNameEntry::first); it != event_names.end()) {
        return std::string(it->second);
    }
    return "unknown";
}

static std::optional<HookEvent> string_to_hook_event(std::string_view name) {
    if (const auto *const it = std::ranges::find(event_names, name, &EventNameEntry::second); it != event_names.end()) {
        return it->first;
    }
    return std::nullopt;
}

// ── ISO 8601 timestamp ──────────────────────────

static std::string iso8601_now() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&time_t_now, &tm_buf);
    std::array<char, 32> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return {buf.data()};
}

// ── Hook discovery ──────────────────────────────

void HookManager::load_from_directories(const std::vector<std::string> &directories) {
    hooks_.clear();

    for (const auto &dir : directories) {
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            continue;
        }

        for (const auto &entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_directory()) {
                continue;
            }

            auto event_name = entry.path().filename().string();
            auto event = string_to_hook_event(event_name);
            if (!event.has_value()) {
                spdlog::debug("Ignoring unknown hook event directory: {}", event_name);
                continue;
            }

            std::vector<HookDef> event_hooks;
            for (const auto &hook_file : std::filesystem::directory_iterator(entry.path())) {
                if (!hook_file.is_regular_file()) {
                    continue;
                }

                auto perms = hook_file.status().permissions();
                if ((perms & std::filesystem::perms::owner_exec) == std::filesystem::perms::none) {
                    spdlog::debug("Skipping non-executable hook: {}", hook_file.path().string());
                    continue;
                }

                event_hooks.push_back(HookDef{
                    .path = hook_file.path().string(),
                    .event = *event,
                    .filename = hook_file.path().filename().string(),
                });
            }

            std::ranges::sort(event_hooks, [](const HookDef &a, const HookDef &b) { return a.filename < b.filename; });

            auto &existing = hooks_[*event];
            for (const auto &hook : event_hooks) {
                if (auto shadowed = std::ranges::find(existing, hook.filename, &HookDef::filename); shadowed != existing.end()) {
                    spdlog::debug("Hook '{}' for '{}' overridden by {}", hook.filename, event_name, hook.path);
                    existing.erase(shadowed);
                }
            }
            existing.insert(existing.end(), std::make_move_iterator(event_hooks.begin()), std::make_move_iterator(event_hooks.end()));
        }
    }

    // Log discovery results
    for (const auto &[event, hooks] : hooks_) {
        if (!hooks.empty()) {
            spdlog::info("Discovered {} hook(s) for event '{}'", hooks.size(), hook_event_to_string(event));
        }
    }
}

// ── Hook execution ──────────────────────────────

static HookResult execute_hook(const HookDef &hook, const json &context) {
    constexpr int hook_timeout_seconds = 5;
    auto subprocess_result = run_subprocess({
        .command = hook.path,
        .stdin_data = context.dump(),
        .timeout = std::chrono::seconds(hook_timeout_seconds),
        .use_shell = false,
    });

    if (subprocess_result.timed_out) {
        spdlog::warn("[{}] Hook timed out after {}s", hook.filename, hook_timeout_seconds);
    }

    return {
        .exit_code = subprocess_result.exit_code,
        .stderr_output = std::move(subprocess_result.stderr_output),
        .timed_out = subprocess_result.timed_out,
    };
}

// ── Hook dispatch ───────────────────────────────

DispatchResult HookManager::dispatch(HookEvent event, const json &context) const {
    auto it = hooks_.find(event);
    if (it == hooks_.end() || it->second.empty()) {
        return {};
    }

    bool is_blocking_event = (event == HookEvent::before_tool_call);

    for (const auto &hook : it->second) {
        spdlog::debug("Dispatching hook '{}' for event '{}'", hook.filename, hook_event_to_string(event));
        auto result = execute_hook(hook, context);

        if (!result.stderr_output.empty()) {
            spdlog::warn("[{}] {}", hook.filename, result.stderr_output);
        }

        if (result.exit_code != 0) {
            if (is_blocking_event) {
                spdlog::info("Hook '{}' blocked {} (exit code {})", hook.filename, hook_event_to_string(event), result.exit_code);
                return {
                    .allowed = false,
                    .blocked_by = hook.filename,
                    .block_reason = result.stderr_output,
                };
            }
            spdlog::warn("Hook '{}' for '{}' exited with code {}", hook.filename, hook_event_to_string(event), result.exit_code);
        }
    }

    return {};
}

size_t HookManager::hook_count(HookEvent event) const {
    auto it = hooks_.find(event);
    return (it != hooks_.end()) ? it->second.size() : 0;
}

size_t HookManager::total_hooks() const {
    size_t total = 0;
    for (const auto &[_, hooks] : hooks_) {
        total += hooks.size();
    }
    return total;
}

// ── Context builders ────────────────────────────

static json build_tool_call_context(HookEvent event, const std::string &tool_name, const json &tool_input, const std::string &tool_result, bool is_error) {
    json ctx = {{"event", hook_event_to_string(event)}, {"timestamp", iso8601_now()}, {"tool_name", tool_name}, {"tool_input", tool_input}};
    if (event == HookEvent::after_tool_call) {
        ctx["tool_result"] = tool_result;
        ctx["is_error"] = is_error;
    }
    return ctx;
}

json build_before_tool_call_context(const std::string &tool_name, const json &tool_input) {
    return build_tool_call_context(HookEvent::before_tool_call, tool_name, tool_input, {}, false);
}

json build_after_tool_call_context(const std::string &tool_name, const json &tool_input, const std::string &tool_result, bool is_error) {
    return build_tool_call_context(HookEvent::after_tool_call, tool_name, tool_input, tool_result, is_error);
}

json build_message_context(HookEvent event, const std::string &role, const std::string &content) {
    return {{"event", hook_event_to_string(event)}, {"timestamp", iso8601_now()}, {"role", role}, {"content", content}};
}

json build_session_context(HookEvent event, const std::string &session_id, size_t message_count) {
    json ctx = {{"event", hook_event_to_string(event)}, {"timestamp", iso8601_now()}, {"session_id", session_id}};
    if (event == HookEvent::session_end) {
        ctx["message_count"] = message_count;
    }
    return ctx;
}

void dispatch_session_start(HookManager *hook_manager, const std::string &session_id, size_t message_count) {
    if (hook_manager == nullptr || session_id.empty()) {
        return;
    }
    (void)hook_manager->dispatch(HookEvent::session_start, build_session_context(HookEvent::session_start, session_id, message_count));
}

void dispatch_session_end(HookManager *hook_manager, const std::string &session_id, size_t message_count) {
    if (hook_manager == nullptr || session_id.empty()) {
        return;
    }
    (void)hook_manager->dispatch(HookEvent::session_end, build_session_context(HookEvent::session_end, session_id, message_count));
}

} // namespace orangutan
