#include "features/hooks/hook-manager.hpp"

#include <exec/any_sender_of.hpp>

#include "infra/execution/sender-utils.hpp"
#include "infra/subprocess/subprocess.hpp"
#include "infra/time/local-format.hpp"

#include <exception>
#include <filesystem>
#include <memory>
#include <spdlog/spdlog.h>
#include <span>
#include <string_view>
#include <magic_enum/magic_enum.hpp>

namespace orangutan {

// ── ISO 8601 timestamp ──────────────────────────

static std::string iso8601_now() {
    return time::current_local_iso8601_timestamp();
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
            auto event = magic_enum::enum_cast<HookEvent>(event_name);
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

            std::ranges::sort(event_hooks, [](const HookDef &a, const HookDef &b) {
                return a.filename < b.filename;
            });

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
            spdlog::info("Discovered {} hook(s) for event '{}'", hooks.size(), magic_enum::enum_name(event));
        }
    }
}

// ── Hook execution ──────────────────────────────

static auto execute_hook_sender(const HookDef &hook, json context) {
    constexpr int hook_timeout_seconds = 5;
    auto hook_path = hook.path;
    auto hook_filename = hook.filename;

    return stdexec::just(std::move(context)) | stdexec::then([](const json &active_context) {
               return active_context.dump();
           }) |
           stdexec::let_value([hook_path = std::move(hook_path), hook_timeout_seconds](std::string stdin_data) mutable {
               return run_subprocess_sender({
                   .command = std::move(hook_path),
                   .stdin_data = std::move(stdin_data),
                   .timeout = std::chrono::seconds(hook_timeout_seconds),
                   .use_shell = false,
               });
           }) |
           stdexec::then([hook_filename = std::move(hook_filename), hook_timeout_seconds](SubprocessResult subprocess_result) {
               if (subprocess_result.timed_out) {
                   spdlog::warn("[{}] Hook timed out after {}s", hook_filename, hook_timeout_seconds);
               }

               return HookResult{
                   .exit_code = subprocess_result.exit_code,
                   .stderr_output = std::move(subprocess_result.stderr_output),
                   .timed_out = subprocess_result.timed_out,
               };
           });
}

template <class... Ts>
using any_sender_of = exec::any_receiver_ref<stdexec::completion_signatures<Ts...>>::template any_sender<>;

using dispatch_sender_t = any_sender_of<stdexec::set_value_t(DispatchResult), stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

static std::optional<DispatchResult> process_hook_result(const HookDef &hook, HookEvent event, bool is_blocking_event, HookResult result) {
    if (!result.stderr_output.empty()) {
        spdlog::warn("[{}] {}", hook.filename, result.stderr_output);
    }

    if (result.exit_code == 0) {
        return std::nullopt;
    }

    if (is_blocking_event) {
        spdlog::info("Hook '{}' blocked {} (exit code {})", hook.filename, magic_enum::enum_name(event), result.exit_code);
        return DispatchResult{
            .allowed = false,
            .blocked_by = hook.filename,
            .block_reason = result.stderr_output,
        };
    }

    spdlog::warn("Hook '{}' for '{}' exited with code {}", hook.filename, magic_enum::enum_name(event), result.exit_code);
    return std::nullopt;
}

static dispatch_sender_t dispatch_hooks_sender(std::span<const HookDef> hooks, size_t index, HookEvent event, const std::shared_ptr<const json> &context, bool is_blocking_event) {
    if (index >= hooks.size()) {
        return stdexec::just(DispatchResult{});
    }

    const HookDef &hook = hooks[index];

    return execute_hook_sender(hook, *context) | stdexec::then([hook, event, is_blocking_event](HookResult result) {
               spdlog::debug("Dispatching hook '{}' for event '{}'", hook.filename, magic_enum::enum_name(event));
               return process_hook_result(hook, event, is_blocking_event, std::move(result));
           }) |
           stdexec::let_value([hooks, index, event, context, is_blocking_event](std::optional<DispatchResult> blocked_result) -> dispatch_sender_t {
               if (blocked_result.has_value()) {
                   return stdexec::just(std::move(*blocked_result));
               }
               return dispatch_hooks_sender(hooks, index + 1, event, context, is_blocking_event);
           });
}

// ── Hook dispatch ───────────────────────────────

DispatchResult HookManager::dispatch(HookEvent event, const json &context) const {
    auto it = hooks_.find(event);
    if (it == hooks_.end() || it->second.empty()) {
        return {};
    }

    bool is_blocking_event = (event == HookEvent::before_tool_call);
    auto context_ptr = std::make_shared<json>(context);
    auto pipeline = dispatch_hooks_sender(it->second, 0, event, std::move(context_ptr), is_blocking_event);
    auto [result] = execution::sync_wait_or_throw(std::move(pipeline), "hook dispatch pipeline");
    return result;
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

static json build_tool_call_context(HookEvent event, std::string_view tool_name, const json &tool_input, std::string_view tool_result, bool is_error) {
    json ctx = {{"event", magic_enum::enum_name(event)}, {"timestamp", iso8601_now()}, {"tool_name", tool_name}, {"tool_input", tool_input}};
    if (event == HookEvent::after_tool_call) {
        ctx["tool_result"] = tool_result;
        ctx["is_error"] = is_error;
    }
    return ctx;
}

json build_before_tool_call_context(std::string_view tool_name, const json &tool_input) {
    return build_tool_call_context(HookEvent::before_tool_call, tool_name, tool_input, {}, false);
}

json build_after_tool_call_context(std::string_view tool_name, const json &tool_input, std::string_view tool_result, bool is_error) {
    return build_tool_call_context(HookEvent::after_tool_call, tool_name, tool_input, tool_result, is_error);
}

json build_message_context(HookEvent event, std::string_view role, std::string_view content) {
    return {{"event", magic_enum::enum_name(event)}, {"timestamp", iso8601_now()}, {"role", role}, {"content", content}};
}

json build_session_context(HookEvent event, std::string_view session_id, size_t message_count) {
    json ctx = {{"event", magic_enum::enum_name(event)}, {"timestamp", iso8601_now()}, {"session_id", session_id}};
    if (event == HookEvent::session_end) {
        ctx["message_count"] = message_count;
    }
    return ctx;
}

void dispatch_session_start(HookManager *hook_manager, std::string_view session_id, size_t message_count) {
    if (hook_manager == nullptr || session_id.empty()) {
        return;
    }
    static_cast<void>(hook_manager->dispatch(HookEvent::session_start, build_session_context(HookEvent::session_start, session_id, message_count)));
}

void dispatch_session_end(HookManager *hook_manager, std::string_view session_id, size_t message_count) {
    if (hook_manager == nullptr || session_id.empty()) {
        return;
    }
    static_cast<void>(hook_manager->dispatch(HookEvent::session_end, build_session_context(HookEvent::session_end, session_id, message_count)));
}

} // namespace orangutan
