#include "app/single-shot.hpp"

#include "app/history-events.hpp"
#include "app/session-workflow.hpp"
#include "features/automation/runtime.hpp"

#include <optional>
#include <ostream>

namespace orangutan::app {

    namespace {

        std::string resolve_active_model(const Provider &provider, const std::string &configured_model) {
            const auto current_model = provider.current_model();
            return current_model.empty() ? configured_model : current_model;
        }

        json done_event(const std::string &session_id) {
            return {
                {"type", "done"},
                {"session_id", session_id},
            };
        }

        void emit_single_message_error(bool event_stream, const JsonEmitter &emit, std::ostream &error_stream, const std::string &message) {
            if (event_stream) {
                emit({
                    {"type", "error"},
                    {"message", message},
                });
                return;
            }

            error_stream << "Error: " << message << '\n';
        }

        auto make_stream_event_emitter(const JsonEmitter &emit) {
            return [&emit](const std::string &event_type, const json &data) {
                if (event_type == "text_delta") {
                    emit({
                        {"type", "assistant_delta"},
                        {"text", data.value("text", "")},
                    });
                    return;
                }

                if (event_type == "tool_call_start") {
                    emit({
                        {"type", "tool_call_started"},
                        {"id", data.value("id", "")},
                        {"name", data.value("name", "")},
                        {"input", data.value("input", json::object())},
                    });
                }
            };
        }

        auto make_tool_event_emitter(const JsonEmitter &emit) {
            return [&emit](const std::string &event_type, const ToolUseBlock &call, const ToolResultBlock *result) {
                if (event_type == "tool_started") {
                    emit({
                        {"type", "tool_started"},
                        {"id", call.id},
                        {"name", call.name},
                        {"input", call.input},
                    });
                    return;
                }

                if (event_type == "tool_finished" && result != nullptr) {
                    emit({
                        {"type", "tool_finished"},
                        {"id", call.id},
                        {"name", call.name},
                        {"input", call.input},
                        {"output", result->content},
                        {"is_error", result->is_error},
                        {"details", build_edit_details(call)},
                    });
                }
            };
        }

        std::optional<std::string> run_completion_resume_message_impl(AgentLoop &agent, const std::string &message, std::string_view agent_key,
                                                                      automation::Runtime *automation_runtime, const CompletionResumePostRunCallback &post_run,
                                                                      bool suppress_human_output) {
            std::optional<std::string> post_run_error;
            automation::with_agent_execution_lease(automation_runtime, agent_key, [&] {
                std::string reply;
                if (suppress_human_output) {
                    reply = agent.run(message, [](const std::string &, const json &) {}, [](const std::string &, const ToolUseBlock &, const ToolResultBlock *) {});
                } else {
                    reply = agent.run(message);
                }
                if (post_run) {
                    post_run_error = post_run(reply);
                }
            });
            return post_run_error;
        }

        void run_single_message_agent(AgentLoop &agent, const std::string &message, bool event_stream, const std::string &current_session_id, const JsonEmitter &emit,
                                      const std::string &agent_key, automation::Runtime *automation_runtime) {
            automation::with_agent_execution_lease(automation_runtime, agent_key, [&] {
                if (!event_stream) {
                    static_cast<void>(agent.run(message));
                    return;
                }

                if (!current_session_id.empty()) {
                    emit(make_session_event_json("session_resumed", current_session_id, "resumed"));
                }

                const auto stream_event = make_stream_event_emitter(emit);
                const auto tool_event = make_tool_event_emitter(emit);
                static_cast<void>(agent.run(message, stream_event, tool_event));
            });
        }

        void maybe_persist_single_message_session(AgentLoop &agent, const Provider &provider, SessionStore &session_store, const Config &cfg, std::string &current_session_id,
                                                  const std::string &configured_model, const std::string &scope_key, const std::string &agent_key, bool event_stream,
                                                  const JsonEmitter &emit, std::ostream &error_stream) {
            if (!cfg.auto_save || agent.history().empty()) {
                return;
            }

            const auto active_model = resolve_active_model(provider, configured_model);
            const auto metadata = make_cli_session_metadata(active_model, scope_key, agent_key);
            const bool has_existing_session = !current_session_id.empty();
            if (has_existing_session) {
                session_store.update(current_session_id, agent.history(), metadata);
            } else {
                current_session_id = session_store.save(agent.history(), metadata);
            }

            if (event_stream) {
                emit(make_session_event_json("session_saved", current_session_id, has_existing_session ? "updated" : "auto_saved"));
                return;
            }

            error_stream << (has_existing_session ? "Session updated: " : "Auto-saved session: ") << current_session_id << " (use -r " << current_session_id << " to resume)\n";
        }

    } // namespace

    std::optional<std::string> run_completion_resume_message(AgentLoop &agent, const std::string &message, std::string_view agent_key, automation::Runtime *automation_runtime,
                                                             const CompletionResumePostRunCallback &post_run, bool suppress_human_output) {
        try {
            return run_completion_resume_message_impl(agent, message, agent_key, automation_runtime, post_run, suppress_human_output);
        } catch (const std::exception &e) {
            return e.what();
        } catch (...) {
            return "background completion resume failed";
        }
    }

    void emit_session_history_dump(const std::vector<Message> &history, const std::string &current_session_id, const JsonEmitter &emit) {
        emit(make_session_event_json("session_resumed", current_session_id, "resumed"));
        for (const auto &event : build_session_history_events(history)) {
            emit(event);
        }
        emit(done_event(current_session_id));
    }

    int run_single_message(AgentLoop &agent, const Provider &provider, SessionStore &session_store, const Config &cfg, const std::string &message, bool event_stream,
                           std::string &current_session_id, const std::string &configured_model, const std::string &scope_key, const std::string &agent_key,
                           const JsonEmitter &emit, std::ostream &error_stream, automation::Runtime *automation_runtime) {
        try {
            run_single_message_agent(agent, message, event_stream, current_session_id, emit, agent_key, automation_runtime);
        } catch (const std::exception &e) {
            emit_single_message_error(event_stream, emit, error_stream, e.what());
            return 1;
        }

        maybe_persist_single_message_session(agent, provider, session_store, cfg, current_session_id, configured_model, scope_key, agent_key, event_stream, emit, error_stream);

        if (event_stream) {
            emit(done_event(current_session_id));
        }
        return 0;
    }

} // namespace orangutan::app
