#include "tools/background/background-completion.hpp"

#include "automation/automation-types.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/sender-utils.hpp"
#include "utils/utf8.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace orangutan::tools {
    namespace {

        constexpr std::string_view completion_message_type = "background_process_completion";
        constexpr std::string_view completion_resume_failure_type = "background_process_completion_resume_failure";
        constexpr std::string_view inbox_source_kind = "background_process";
        constexpr std::string_view default_agent_key = "default";
        constexpr std::size_t max_output_summary_bytes = 2048;
        constexpr std::size_t max_runtime_key_chars = 256;
        constexpr std::size_t max_agent_key_chars = 128;
        constexpr std::size_t max_process_id_chars = 128;
        constexpr std::size_t max_command_chars = 2048;
        constexpr std::size_t max_working_dir_chars = 1024;
        constexpr std::size_t max_failure_reason_chars = 512;
        constexpr std::size_t max_failure_payload_bytes = background_completion_payload_max_bytes * 2;
        constexpr std::size_t max_title_command_chars = 80;
        constexpr std::size_t max_inbox_title_chars = 160;

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

        std::string scrub_and_bound_title(std::string_view title) {
            return utf8::truncate_valid_prefix(scrub_tool_output(utf8::sanitize(title)), max_inbox_title_chars, true);
        }

        std::string clip_command(std::string_view command) {
            return utf8::sanitize_and_truncate_valid_prefix(command, max_title_command_chars, true);
        }

        nlohmann::json summarize_output(const BackgroundProcessOutputMetadata &output) {
            const std::string sanitized_tail = utf8::sanitize(output.tail);
            std::string tail = utf8::truncate_valid_suffix(sanitized_tail, max_output_summary_bytes);
            bool truncated = output.truncated;
            truncated = truncated || tail.size() < sanitized_tail.size();

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
                return utf8::sanitize_and_truncate_valid_prefix(it->second, background_completion_prompt_max_chars, true);
            }
            return std::nullopt;
        }

        nlohmann::json build_completion_payload(const BackgroundProcessCompletionEvent &event, std::string_view runtime_key, std::string_view agent_key) {
            nlohmann::json payload = {
                {"type", completion_message_type},
                {"runtime_key", utf8::sanitize_and_truncate_valid_prefix(runtime_key, max_runtime_key_chars, true)},
                {"agent_key", utf8::sanitize_and_truncate_valid_prefix(agent_key, max_agent_key_chars, true)},
                {"process_id", utf8::sanitize_and_truncate_valid_prefix(event.process_id, max_process_id_chars, true)},
                {"command", utf8::sanitize_and_truncate_valid_prefix(event.command, max_command_chars, true)},
                {"working_dir", utf8::sanitize_and_truncate_valid_prefix(event.working_dir, max_working_dir_chars, true)},
                {"pid", event.pid},
                {"status", process_status(event)},
                {"kill_requested", event.kill_requested},
                {"stdout", summarize_output(event.stdout)},
                {"stderr", summarize_output(event.stderr)},
                {"on_complete", {{"mode", completion_mode(event.metadata)}}},
            };
            payload["exit_code"] = event.exit_code.has_value() ? nlohmann::json(*event.exit_code) : nlohmann::json(nullptr);
            payload["signal_number"] = event.signal_number.has_value() ? nlohmann::json(*event.signal_number) : nlohmann::json(nullptr);
            if (const auto prompt = completion_prompt(event.metadata); prompt.has_value()) {
                payload["on_complete"]["prompt"] = *prompt;
            }
            return payload;
        }

        std::string inbox_title_for_event(const BackgroundProcessCompletionEvent &event) {
            return scrub_and_bound_title("Background process " + process_status(event) + ": " + clip_command(event.command));
        }

        std::string failure_reason_or_default(const std::optional<std::string> &reason) {
            if (!reason.has_value() || reason->empty()) {
                return "resume callback returned an unspecified failure";
            }
            return utf8::sanitize_and_truncate_valid_prefix(*reason, max_failure_reason_chars, true);
        }

        bool insert_inbox_item(const BackgroundCompletionRuntimeBindings &bindings, const automation::InboxItem &item, std::string_view process_id) {
            if (!bindings.supports_completion_routing()) {
                return false;
            }

            try {
                bindings.inbox_callback()(item);
                return true;
            } catch (const std::exception &ex) {
                spdlog::warn("background completion inbox callback threw for process {}: {}", process_id, ex.what());
            } catch (...) {
                spdlog::warn("background completion inbox callback threw for process {} with an unknown exception", process_id);
            }
            return false;
        }

    } // namespace

    BackgroundCompletionDispatcher::BackgroundCompletionDispatcher(const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }

        runtime_key_ = tool_context->runtime_key;
        agent_key_ = tool_context->agent_key.empty() ? std::string(default_agent_key) : tool_context->agent_key;
        background_completion_runtime_ = tool_context->background_completion_runtime;
        if (background_completion_runtime_ != nullptr) {
            supports_completion_routing_ = background_completion_runtime_->supports_completion_routing();
            supports_resume_callback_ = background_completion_runtime_->supports_resume_callback();
        }
    }

    bool BackgroundCompletionDispatcher::supports_completion_routing() const {
        return supports_completion_routing_;
    }

    bool BackgroundCompletionDispatcher::supports_resume_callback() const {
        return supports_resume_callback_;
    }

    void BackgroundCompletionDispatcher::dispatch(const BackgroundProcessCompletionEvent &event) const {
        const auto bindings = background_completion_runtime_;
        if (bindings == nullptr) {
            return;
        }

        if (!bindings->supports_completion_routing()) {
            return;
        }

        const auto payload = build_completion_payload(event, runtime_key_, agent_key_);
        const auto payload_text = scrub_tool_output(payload.dump(2));
        if (payload_text.size() > background_completion_payload_max_bytes) {
            spdlog::warn("background completion payload exceeded bounded size for process {}", event.process_id);
            return;
        }
        const auto persisted_payload = nlohmann::json::parse(payload_text);
        const auto requested_completion_mode = completion_mode(event.metadata);

        const auto insert_resume_failure_note = [&](std::string_view reason) {
            const auto failure_payload = nlohmann::json{
                {"type", completion_resume_failure_type},
                {"runtime_key", utf8::sanitize_and_truncate_valid_prefix(runtime_key_, max_runtime_key_chars, true)},
                {"agent_key", utf8::sanitize_and_truncate_valid_prefix(agent_key_, max_agent_key_chars, true)},
                {"process_id", utf8::sanitize_and_truncate_valid_prefix(event.process_id, max_process_id_chars, true)},
                {"reason", utf8::sanitize_and_truncate_valid_prefix(reason, max_failure_reason_chars, true)},
                {"completion", persisted_payload},
            };
            const auto failure_body = scrub_tool_output(failure_payload.dump(2));
            if (failure_body.size() > max_failure_payload_bytes) {
                spdlog::warn("background completion failure payload exceeded bounded size for process {}", event.process_id);
                return;
            }
            static_cast<void>(insert_inbox_item(*bindings,
                                                automation::InboxItem{
                                                    .agent_key = agent_key_,
                                                    .source_kind = std::string(inbox_source_kind),
                                                    .source_run_id = event.process_id,
                                                    .title = scrub_and_bound_title("Background completion resume failed: " + clip_command(event.command)),
                                                    .body = failure_body,
                                                    .created_at = automation::to_unix_seconds(automation::Clock::now()),
                                                },
                                                event.process_id));
        };

        auto pipeline = stdexec::just() | stdexec::then([&]() -> bool {
                            return insert_inbox_item(*bindings,
                                                     automation::InboxItem{
                                                         .agent_key = agent_key_,
                                                         .source_kind = std::string(inbox_source_kind),
                                                         .source_run_id = event.process_id,
                                                         .title = inbox_title_for_event(event),
                                                         .body = payload_text,
                                                         .created_at = automation::to_unix_seconds(automation::Clock::now()),
                                                     },
                                                     event.process_id);
                        }) |
                        stdexec::then([&](bool inserted) {
                            if (!inserted || requested_completion_mode != "resume") {
                                return;
                            }

                            if (!bindings->supports_resume_callback()) {
                                insert_resume_failure_note("resume requested, but no background completion resume callback is registered");
                                return;
                            }

                            try {
                                const auto error = bindings->resume_callback()(payload_text);
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
                        });

        try {
            static_cast<void>(execution::sync_wait_or_throw(pipeline, "background completion dispatch pipeline"));
        } catch (const std::exception &ex) {
            spdlog::error("background completion dispatch pipeline failed for process {}: {}", event.process_id, ex.what());
        } catch (...) {
            spdlog::error("background completion dispatch pipeline failed for process {} with non-standard exception", event.process_id);
        }
    }

} // namespace orangutan::tools
