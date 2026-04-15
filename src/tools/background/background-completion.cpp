#include "tools/background/background-completion.hpp"

#include "automation/repository.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/sender-utils.hpp"
#include "utils/utf8-policy.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace orangutan::tools {
    namespace {

        constexpr std::string_view COMPLETION_MESSAGE_TYPE = "background_process_completion";
        constexpr std::string_view COMPLETION_RESUME_FAILURE_TYPE = "background_process_completion_resume_failure";
        constexpr std::string_view BACKGROUND_COMPLETION_AUTOMATION_ID = "background-completion";
        constexpr std::string_view BACKGROUND_COMPLETION_TARGET = "background";
        constexpr std::string_view DEFAULT_AGENT_KEY = "default";
        constexpr std::size_t MAX_OUTPUT_SUMMARY_BYTES = 2048;
        constexpr std::size_t MAX_RUNTIME_KEY_CHARS = 256;
        constexpr std::size_t MAX_AGENT_KEY_CHARS = 128;
        constexpr std::size_t MAX_PROCESS_ID_CHARS = 128;
        constexpr std::size_t MAX_COMMAND_CHARS = 2048;
        constexpr std::size_t MAX_WORKING_DIR_CHARS = 1024;
        constexpr std::size_t MAX_FAILURE_REASON_CHARS = 512;
        constexpr std::size_t MAX_FAILURE_PAYLOAD_BYTES = BACKGROUND_COMPLETION_PAYLOAD_MAX_BYTES * 2;
        constexpr std::size_t MAX_TITLE_COMMAND_CHARS = 80;
        constexpr std::size_t MAX_DELIVERY_TITLE_CHARS = 160;

        std::string process_status(const BackgroundProcessCompletionEvent &event) {
            switch (event.terminal_status) {
                case background_process_terminal_status::signaled:
                    return event.kill_requested ? "killed" : "signaled";
                case background_process_terminal_status::exited:
                    if (event.exit_code.has_value() && *event.exit_code == 0) {
                        return "completed";
                    }
                    return "failed";
                case background_process_terminal_status::unknown:
                default:
                    return "unknown";
            }
        }

        std::string scrub_and_bound_title(std::string_view title) {
            const auto canonicalized = utf8_policy::canonicalize(scrub_tool_output(title), utf8_policy::display_policy(MAX_DELIVERY_TITLE_CHARS, true));
            return canonicalized.has_value() ? std::move(canonicalized->value) : std::string{};
        }

        std::string clip_command(std::string_view command) {
            const auto canonicalized = utf8_policy::canonicalize(command, utf8_policy::display_policy(MAX_TITLE_COMMAND_CHARS, true));
            return canonicalized.has_value() ? std::move(canonicalized->value) : std::string{};
        }

        nlohmann::json summarize_output(const BackgroundProcessOutputMetadata &output) {
            auto suffix_policy = utf8_policy::display_policy();
            suffix_policy.boundary = utf8_policy::bound_mode::suffix;
            suffix_policy.max_bytes = MAX_OUTPUT_SUMMARY_BYTES;

            const auto canonicalized = utf8_policy::canonicalize(output.tail, suffix_policy);
            std::string tail = canonicalized.has_value() ? std::move(canonicalized->value) : std::string{};
            bool truncated = output.truncated;
            truncated = truncated || (canonicalized.has_value() && canonicalized->truncated);

            return {
                {"tail", std::move(tail)},
                {"total_bytes", output.total_bytes},
                {"truncated", truncated},
            };
        }

        std::string completion_mode(const std::map<std::string, std::string> &metadata) {
            if (const auto it = metadata.find(std::string(BACKGROUND_COMPLETION_MODE_METADATA_KEY)); it != metadata.end() && !it->second.empty()) {
                return it->second;
            }
            return "inbox";
        }

        std::optional<std::string> completion_prompt(const std::map<std::string, std::string> &metadata) {
            if (const auto it = metadata.find(std::string(BACKGROUND_COMPLETION_PROMPT_METADATA_KEY)); it != metadata.end()) {
                const auto canonicalized = utf8_policy::canonicalize(it->second, utf8_policy::display_policy(BACKGROUND_COMPLETION_PROMPT_MAX_CHARS, true));
                if (!canonicalized.has_value()) {
                    return std::nullopt;
                }
                return std::optional<std::string>{std::move(canonicalized->value)};
            }
            return std::nullopt;
        }

        nlohmann::json build_completion_payload(const BackgroundProcessCompletionEvent &event, std::string_view runtime_key, std::string_view agent_key) {
            const auto canonical_runtime_key = utf8_policy::canonicalize(runtime_key, utf8_policy::display_policy(MAX_RUNTIME_KEY_CHARS, true));
            const auto canonical_agent_key = utf8_policy::canonicalize(agent_key, utf8_policy::display_policy(MAX_AGENT_KEY_CHARS, true));
            const auto canonical_process_id = utf8_policy::canonicalize(event.process_id, utf8_policy::display_policy(MAX_PROCESS_ID_CHARS, true));
            const auto canonical_command = utf8_policy::canonicalize(event.command, utf8_policy::display_policy(MAX_COMMAND_CHARS, true));
            const auto canonical_working_dir = utf8_policy::canonicalize(event.working_dir, utf8_policy::display_policy(MAX_WORKING_DIR_CHARS, true));

            nlohmann::json payload = {
                {"type", COMPLETION_MESSAGE_TYPE},
                {"runtime_key", canonical_runtime_key.has_value() ? std::move(canonical_runtime_key->value) : std::string{}},
                {"agent_key", canonical_agent_key.has_value() ? std::move(canonical_agent_key->value) : std::string{}},
                {"process_id", canonical_process_id.has_value() ? std::move(canonical_process_id->value) : std::string{}},
                {"command", canonical_command.has_value() ? std::move(canonical_command->value) : std::string{}},
                {"working_dir", canonical_working_dir.has_value() ? std::move(canonical_working_dir->value) : std::string{}},
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
            const auto canonicalized = utf8_policy::canonicalize(*reason, utf8_policy::display_policy(MAX_FAILURE_REASON_CHARS, true));
            return canonicalized.has_value() ? std::move(canonicalized->value) : std::string{};
        }

        bool insert_delivery_record(const BackgroundCompletionRuntimeBindings &bindings, const automation::DeliveryRecord &delivery, std::string_view process_id) {
            if (!bindings.supports_completion_routing()) {
                return false;
            }

            try {
                bindings.delivery_callback()(delivery);
                return true;
            } catch (const std::exception &ex) {
                spdlog::warn("background completion delivery callback threw for process {}: {}", process_id, ex.what());
            } catch (...) {
                spdlog::warn("background completion delivery callback threw for process {} with an unknown exception", process_id);
            }
            return false;
        }

    } // namespace

    BackgroundCompletionDispatcher::BackgroundCompletionDispatcher(const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }

        runtime_key_ = tool_context->runtime_key;
        agent_key_ = tool_context->agent_key.empty() ? std::string(DEFAULT_AGENT_KEY) : tool_context->agent_key;
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
        if (payload_text.size() > BACKGROUND_COMPLETION_PAYLOAD_MAX_BYTES) {
            spdlog::warn("background completion payload exceeded bounded size for process {}", event.process_id);
            return;
        }
        const auto persisted_payload = nlohmann::json::parse(payload_text);
        const auto requested_completion_mode = completion_mode(event.metadata);

        const auto insert_resume_failure_note = [&](std::string_view reason) {
            const auto canonical_runtime_key = utf8_policy::canonicalize(runtime_key_, utf8_policy::display_policy(MAX_RUNTIME_KEY_CHARS, true));
            const auto canonical_agent_key = utf8_policy::canonicalize(agent_key_, utf8_policy::display_policy(MAX_AGENT_KEY_CHARS, true));
            const auto canonical_process_id = utf8_policy::canonicalize(event.process_id, utf8_policy::display_policy(MAX_PROCESS_ID_CHARS, true));
            const auto canonical_reason = utf8_policy::canonicalize(reason, utf8_policy::display_policy(MAX_FAILURE_REASON_CHARS, true));

            const auto failure_payload = nlohmann::json{
                {"type", COMPLETION_RESUME_FAILURE_TYPE},
                {"runtime_key", canonical_runtime_key.has_value() ? std::move(canonical_runtime_key->value) : std::string{}},
                {"agent_key", canonical_agent_key.has_value() ? std::move(canonical_agent_key->value) : std::string{}},
                {"process_id", canonical_process_id.has_value() ? std::move(canonical_process_id->value) : std::string{}},
                {"reason", canonical_reason.has_value() ? std::move(canonical_reason->value) : std::string{}},
                {"completion", persisted_payload},
            };
            const auto failure_body = scrub_tool_output(failure_payload.dump(2));
            if (failure_body.size() > MAX_FAILURE_PAYLOAD_BYTES) {
                spdlog::warn("background completion failure payload exceeded bounded size for process {}", event.process_id);
                return;
            }
            static_cast<void>(insert_delivery_record(*bindings,
                                                     automation::DeliveryRecord{
                                                         .run_id = event.process_id,
                                                         .automation_id = std::string(BACKGROUND_COMPLETION_AUTOMATION_ID),
                                                         .agent_key = agent_key_,
                                                         .target = std::string(BACKGROUND_COMPLETION_TARGET),
                                                         .status = "resume_failed",
                                                         .title = scrub_and_bound_title("Background completion resume failed: " + clip_command(event.command)),
                                                         .body = failure_body,
                                                         .created_at = automation::to_unix_seconds(automation::Clock::now()),
                                                     },
                                                     event.process_id));
        };

        auto pipeline = stdexec::just() | stdexec::then([&]() -> bool {
                            return insert_delivery_record(*bindings,
                                                          automation::DeliveryRecord{
                                                              .run_id = event.process_id,
                                                              .automation_id = std::string(BACKGROUND_COMPLETION_AUTOMATION_ID),
                                                              .agent_key = agent_key_,
                                                              .target = std::string(BACKGROUND_COMPLETION_TARGET),
                                                              .status = process_status(event),
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
