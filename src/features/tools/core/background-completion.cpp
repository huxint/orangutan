#include "features/tools/core/background-completion.hpp"

#include "features/automation/runtime.hpp"
#include "features/automation/types.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace orangutan {
namespace {

constexpr std::string_view completion_message_type = "background_process_completion";
constexpr std::string_view completion_resume_failure_type = "background_process_completion_resume_failure";
constexpr std::string_view inbox_source_kind = "background_process";
constexpr std::string_view default_agent_key = "default";
constexpr size_t max_output_summary_bytes = 2048;
constexpr size_t max_title_command_chars = 80;

std::string process_status(const BackgroundProcessCompletionEvent &event) {
    switch (event.terminal_status) {
        case BackgroundProcessTerminalStatus::signaled:
            return event.kill_requested ? "killed" : "signaled";
        case BackgroundProcessTerminalStatus::exited:
            if (event.exit_code.has_value() && *event.exit_code == 0) {
                return "completed";
            }
            return "failed";
        case BackgroundProcessTerminalStatus::unknown:
        default:
            return "unknown";
    }
}

std::string clip_command(std::string_view command) {
    if (command.size() <= max_title_command_chars) {
        return std::string(command);
    }
    return std::string(command.substr(0, max_title_command_chars)) + "...";
}

json summarize_output(const BackgroundProcessOutputMetadata &output) {
    std::string tail = output.tail;
    bool truncated = output.truncated;
    if (tail.size() > max_output_summary_bytes) {
        tail = tail.substr(tail.size() - max_output_summary_bytes);
        truncated = true;
    }

    return {
        {"tail", std::move(tail)},
        {"total_bytes", output.total_bytes},
        {"truncated", truncated},
    };
}

std::string completion_mode(const std::map<std::string, std::string> &metadata) {
    if (const auto it = metadata.find(std::string(background_completion_mode_metadata_key)); it != metadata.end() && !it->second.empty()) {
        return it->second;
    }
    return "inbox";
}

std::optional<std::string> completion_prompt(const std::map<std::string, std::string> &metadata) {
    if (const auto it = metadata.find(std::string(background_completion_prompt_metadata_key)); it != metadata.end()) {
        return it->second;
    }
    return std::nullopt;
}

json build_completion_payload(const BackgroundProcessCompletionEvent &event, std::string_view runtime_key, std::string_view agent_key) {
    json payload = {
        {"type", completion_message_type},
        {"runtime_key", runtime_key},
        {"agent_key", agent_key},
        {"process_id", event.process_id},
        {"command", event.command},
        {"working_dir", event.working_dir},
        {"pid", event.pid},
        {"status", process_status(event)},
        {"kill_requested", event.kill_requested},
        {"stdout", summarize_output(event.stdout)},
        {"stderr", summarize_output(event.stderr)},
        {"on_complete", {{"mode", completion_mode(event.metadata)}}},
    };
    payload["exit_code"] = event.exit_code.has_value() ? json(*event.exit_code) : json(nullptr);
    payload["signal_number"] = event.signal_number.has_value() ? json(*event.signal_number) : json(nullptr);
    if (const auto prompt = completion_prompt(event.metadata); prompt.has_value()) {
        payload["on_complete"]["prompt"] = *prompt;
    }
    return payload;
}

std::string inbox_title_for_event(const BackgroundProcessCompletionEvent &event) {
    return "Background process " + process_status(event) + ": " + clip_command(event.command);
}

std::string failure_reason_or_default(const std::optional<std::string> &reason) {
    if (!reason.has_value() || reason->empty()) {
        return "resume callback returned an unspecified failure";
    }
    return *reason;
}

} // namespace

BackgroundCompletionDispatcher::BackgroundCompletionDispatcher(const ToolRuntimeContext *tool_context) {
    if (tool_context == nullptr) {
        return;
    }

    runtime_key_ = tool_context->runtime_key;
    agent_key_ = tool_context->agent_key.empty() ? std::string(default_agent_key) : tool_context->agent_key;
    background_completion_runtime_ = tool_context->background_completion_runtime;
}

bool BackgroundCompletionDispatcher::supports_completion_routing() const {
    const auto bindings = background_completion_runtime_.lock();
    if (bindings == nullptr) {
        return false;
    }

    return bindings->snapshot().automation_runtime != nullptr;
}

bool BackgroundCompletionDispatcher::supports_resume_callback() const {
    const auto bindings = background_completion_runtime_.lock();
    if (bindings == nullptr) {
        return false;
    }

    return static_cast<bool>(bindings->snapshot().resume_callback);
}

void BackgroundCompletionDispatcher::dispatch(const BackgroundProcessCompletionEvent &event) const {
    const auto bindings = background_completion_runtime_.lock();
    if (bindings == nullptr) {
        return;
    }

    const auto runtime_snapshot = bindings->snapshot();
    if (runtime_snapshot.automation_runtime == nullptr) {
        return;
    }

    const auto payload = build_completion_payload(event, runtime_key_, agent_key_);
    const auto payload_text = scrub_tool_output(payload.dump(2));
    const auto persisted_payload = json::parse(payload_text);
    auto &store = runtime_snapshot.automation_runtime->store();
    (void)store.insert_inbox(automation::InboxItem{
        .agent_key = agent_key_,
        .source_kind = std::string(inbox_source_kind),
        .source_run_id = event.process_id,
        .title = inbox_title_for_event(event),
        .body = payload_text,
        .created_at = automation::to_unix_seconds(automation::Clock::now()),
    });

    if (completion_mode(event.metadata) != "resume") {
        return;
    }

    const auto insert_resume_failure_note = [&](std::string_view reason) {
        const auto failure_payload = json{
            {"type", completion_resume_failure_type}, {"runtime_key", runtime_key_},   {"agent_key", agent_key_},
            {"process_id", event.process_id},         {"reason", std::string(reason)}, {"completion", persisted_payload},
        };
        (void)store.insert_inbox(automation::InboxItem{
            .agent_key = agent_key_,
            .source_kind = std::string(inbox_source_kind),
            .source_run_id = event.process_id,
            .title = "Background completion resume failed: " + clip_command(event.command),
            .body = scrub_tool_output(failure_payload.dump(2)),
            .created_at = automation::to_unix_seconds(automation::Clock::now()),
        });
    };

    if (!runtime_snapshot.resume_callback) {
        insert_resume_failure_note("resume requested, but no background completion resume callback is registered");
        return;
    }

    try {
        const auto error = runtime_snapshot.resume_callback(payload_text);
        if (error.has_value()) {
            spdlog::warn("background completion resume callback failed for process {}: {}", event.process_id, *error);
            insert_resume_failure_note(failure_reason_or_default(error));
        }
    } catch (const std::exception &ex) {
        spdlog::warn("background completion resume callback threw for process {}: {}", event.process_id, ex.what());
        insert_resume_failure_note(ex.what());
    } catch (...) {
        spdlog::warn("background completion resume callback threw for process {} with an unknown exception", event.process_id);
        insert_resume_failure_note("resume callback threw an unknown exception");
    }
}

} // namespace orangutan
