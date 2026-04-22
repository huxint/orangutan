#include "web/errors.hpp"
#include "web/event-bus.hpp"
#include "web/sse.hpp"
#include "web/web-route-internal.hpp"

#include "agent/agent-loop.hpp"
#include "automation/runtime.hpp"
#include "providers/provider.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/scope-exit.hpp"

#include <spdlog/spdlog.h>

namespace orangutan::web {

    namespace bootstrap = orangutan::bootstrap;

    namespace {

        /// Publish a chat lifecycle event if an event bus is wired up. Keeps the observatory
        /// in sync with per-session activity without the chat client having to do anything.
        void maybe_publish(const WebContext &ctx, std::string_view kind, const std::string &session_id, const std::string &agent_key,
                           nlohmann::json payload = nlohmann::json::object()) {
            if (ctx.event_bus == nullptr) {
                return;
            }
            payload["session_id"] = session_id;
            payload["agent_key"] = agent_key;
            ctx.event_bus->publish(kind, session_id, std::move(payload));
        }

    } // namespace

    void handle_chat(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.config == nullptr) {
            send_error(res, 503, "config_unavailable", "config not available");
            return;
        }
        if (ctx.sessions == nullptr || ctx.sessions_mutex == nullptr) {
            send_error(res, 503, "sessions_unavailable", "session registry not wired");
            return;
        }

        const auto input = parse_body(req, res);
        if (!input.has_value()) {
            return;
        }

        if (!input->contains("message") || !(*input)["message"].is_string()) {
            send_error(res, 400, "invalid_request", "missing or invalid 'message' field");
            return;
        }
        if (!input->contains("agent_key") || !(*input)["agent_key"].is_string()) {
            send_error(res, 400, "invalid_request", "missing or invalid 'agent_key' field");
            return;
        }

        auto message = input->at("message").get<std::string>();
        const auto agent_key = input->at("agent_key").get<std::string>();
        const auto maybe_agent = internal::find_effective_agent(ctx.config, agent_key);
        if (!maybe_agent.has_value()) {
            send_error(res, 404, "agent_not_found", "agent not found");
            return;
        }
        const auto metadata = internal::make_web_session_metadata(agent_key, *maybe_agent);
        std::string session_id;
        if (input->contains("session_id") && !(*input)["session_id"].is_null()) {
            if (!(*input)["session_id"].is_string()) {
                send_error(res, 400, "invalid_request", "invalid 'session_id' field");
                return;
            }
            session_id = input->at("session_id").get<std::string>();
        }

        std::optional<storage::SessionInfo> existing_session;
        if (!session_id.empty()) {
            if (ctx.session_store == nullptr) {
                send_error(res, 503, "store_unavailable", "session store not available");
                return;
            }
            existing_session = internal::find_agent_session(ctx.session_store, agent_key, session_id);
            if (!existing_session.has_value()) {
                send_error(res, 404, "session_not_found", "session not found");
                return;
            }
        }

        if (message.starts_with('/')) {
            if (const auto command_response = internal::handle_web_static_slash_command(message, agent_key, *ctx.config, ctx.session_store,
                                                                                        existing_session, metadata, session_id);
                command_response.handled) {
                internal::send_web_command_stream(res, command_response);
                return;
            }
        }

        if (existing_session.has_value() && internal::session_is_read_only(*existing_session)) {
            send_error(res, 409, "read_only", "channel sessions are read-only in web chat");
            return;
        }

        if (!session_id.empty()) {
            std::scoped_lock lock(*ctx.sessions_mutex);
            if (ctx.sessions->contains(session_id)) {
                send_error(res, 409, "session_active", "session already active");
                return;
            }
        }

        try {
            auto session = std::make_unique<WebSessionState>();
            session->session_id = session_id;
            session->completion_resume_state = std::make_shared<WebCompletionResumeState>();
            session->completion_resume_state->agent_key = agent_key;
            session->completion_resume_state->automation_runtime = ctx.automation_runtime;
            auto *session_ptr = session.get();
            auto approval_event_emitter = std::make_shared<detail::web_approval_event_emitter>();
            auto approval_stream_open = std::make_shared<std::function<bool()>>();
            auto *automation_service = ctx.automation_runtime != nullptr ? &ctx.automation_runtime->service() : nullptr;
            std::mutex *sessions_mutex = ctx.sessions_mutex;
            session->runtime = std::make_unique<bootstrap::AgentRuntimeBundle>(detail::build_web_runtime_bundle(
                *ctx.config, agent_key, ctx.memory_store, &session->session_id, automation_service, ctx.automation_runtime,
                [session_ptr, sessions_mutex, approval_event_emitter, approval_stream_open](const ToolUse &call, const PermissionDecision &decision) {
                    return detail::await_web_approval(*session_ptr, *sessions_mutex, call, decision,
                                                      approval_event_emitter != nullptr ? *approval_event_emitter : detail::web_approval_event_emitter{},
                                                      approval_stream_open != nullptr ? *approval_stream_open : std::function<bool()>{});
                },
                session->completion_resume_state));
            if (session->agent() == nullptr) {
                throw std::runtime_error("failed to initialize web runtime agent");
            }

            if (!session->session_id.empty() && ctx.session_store != nullptr) {
                session->agent()->set_history(ctx.session_store->load(session->session_id));
                session->persisted_message_count = session->agent()->history().size();
            }

            if (message.starts_with('/')) {
                if (const auto command_response =
                        internal::handle_web_runtime_slash_command(message, agent_key, *maybe_agent, ctx.session_store, metadata, *session->runtime, session->session_id);
                    command_response.handled) {
                    internal::send_web_command_stream(res, command_response);
                    return;
                }
            }

            if (session->session_id.empty() && ctx.session_store != nullptr) {
                session->session_id = ctx.session_store->create_empty(metadata);
            }
            if (session->session_id.empty()) {
                session->session_id = "web-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            }

            auto *abort_flag = &session->abort_requested;
            auto *tool_context = &session->runtime->tool_context();
            if (tool_context != nullptr) {
                tool_context->abort_checker = [abort_flag] {
                    return abort_flag->load();
                };
            }

            auto *agent_ptr = session->agent();
            if (session->completion_resume_state != nullptr) {
                std::scoped_lock lock(session->completion_resume_state->mutex);
                session->completion_resume_state->agent = agent_ptr;
            }
            const auto active_session_id = session->session_id;

            {
                std::scoped_lock lock(*ctx.sessions_mutex);
                const auto [inserted_it, inserted] = ctx.sessions->try_emplace(active_session_id, std::move(session));
                if (!inserted) {
                    send_error(res, 409, "session_active", "session already active");
                    return;
                }
                session_ptr = inserted_it->second.get();
            }

            maybe_publish(ctx, "chat.session_started", active_session_id, agent_key, {{"preview", message.substr(0, 140)}});

            auto *store_ptr = ctx.session_store;
            auto *event_bus = ctx.event_bus;
            auto *automation_runtime = ctx.automation_runtime;
            auto *sessions_map = ctx.sessions;
            std::mutex *stream_sessions_mutex = ctx.sessions_mutex;

            prepare_sse_response(res);
            res.set_chunked_content_provider(
                "text/event-stream",
                [agent_ptr, session_ptr, store_ptr, event_bus, captured_session_id = active_session_id, captured_metadata = metadata, message, approval_event_emitter,
                 approval_stream_open, agent_key, automation_runtime, stream_sessions_mutex, sessions_map](std::size_t /*offset*/, httplib::DataSink &sink) -> bool {
                    if (approval_event_emitter != nullptr) {
                        *approval_event_emitter = [&sink](std::string_view event_name, const nlohmann::json &payload) {
                            return write_sse(sink, event_name, payload);
                        };
                    }
                    if (approval_stream_open != nullptr) {
                        *approval_stream_open = [&sink]() {
                            return sink.is_writable == nullptr || sink.is_writable();
                        };
                    }

                    const auto restore_approval_stream_state = utils::scope_exit([session_ptr, approval_event_emitter, approval_stream_open] {
                        session_ptr->running = false;
                        detail::cancel_pending_approval(*session_ptr);
                        if (approval_event_emitter != nullptr) {
                            *approval_event_emitter = {};
                        }
                        if (approval_stream_open != nullptr) {
                            *approval_stream_open = {};
                        }
                    });

                    write_sse(sink, "session", {{"session_id", captured_session_id}});

                    session_ptr->running = true;
                    try {
                        automation::with_agent_execution_lease(automation_runtime, agent_key, [&] {
                            agent_ptr->run(
                                message,
                                [&sink, session_ptr, event_bus, captured_session_id, agent_key](const ProviderEvent &event) {
                                    if (session_ptr->abort_requested) {
                                        return;
                                    }
                                    if (const auto *text = std::get_if<TextDelta>(&event)) {
                                        write_sse(sink, "text", {{"text", text->text}});
                                        if (event_bus != nullptr) {
                                            event_bus->publish("chat.text", captured_session_id,
                                                               {{"session_id", captured_session_id}, {"agent_key", agent_key}, {"text", text->text}});
                                        }
                                    } else if (const auto *thinking = std::get_if<ThinkingDelta>(&event)) {
                                        write_sse(sink, "thinking", {{"thinking", thinking->thinking}});
                                    }
                                },
                                [&sink, session_ptr, event_bus, captured_session_id, agent_key](const std::string &event_type, const ToolUse &call, const ToolResult *result) {
                                    if (session_ptr->abort_requested) {
                                        return;
                                    }
                                    if (event_type == "tool_started" || event_type == "tool_start") {
                                        write_sse(sink, "tool_start", {{"id", call.id}, {"name", call.name}, {"input", call.input}});
                                        if (event_bus != nullptr) {
                                            event_bus->publish("chat.tool_start", captured_session_id,
                                                               {{"session_id", captured_session_id}, {"agent_key", agent_key}, {"tool", call.name}, {"id", call.id}});
                                        }
                                    } else if ((event_type == "tool_finished" || event_type == "tool_end") && result != nullptr) {
                                        write_sse(sink, "tool_end",
                                                  {{"id", call.id}, {"name", call.name}, {"content", result->content}, {"is_error", result->is_error}});
                                        if (event_bus != nullptr) {
                                            event_bus->publish("chat.tool_end", captured_session_id,
                                                               {{"session_id", captured_session_id},
                                                                {"agent_key", agent_key},
                                                                {"tool", call.name},
                                                                {"id", call.id},
                                                                {"is_error", result->is_error}});
                                        }
                                    }
                                });
                        });

                        write_sse(sink, "done", nlohmann::json::object());
                        if (event_bus != nullptr) {
                            event_bus->publish("chat.done", captured_session_id, {{"session_id", captured_session_id}, {"agent_key", agent_key}});
                        }
                    } catch (const std::exception &e) {
                        write_sse(sink, "error", {{"error", e.what()}});
                        if (event_bus != nullptr) {
                            event_bus->publish("chat.error", captured_session_id,
                                               {{"session_id", captured_session_id}, {"agent_key", agent_key}, {"error", e.what()}});
                        }
                    }

                    if (store_ptr != nullptr) {
                        try {
                            const auto &history = agent_ptr->history();
                            if (history.size() > session_ptr->persisted_message_count) {
                                store_ptr->append(captured_session_id, history, session_ptr->persisted_message_count, captured_metadata);
                            } else {
                                store_ptr->update(captured_session_id, history, captured_metadata);
                            }
                            session_ptr->persisted_message_count = history.size();
                        } catch (const std::exception &e) {
                            spdlog::warn("failed to save session {}: {}", captured_session_id, e.what());
                        }
                    }

                    if (session_ptr->completion_resume_state != nullptr) {
                        std::scoped_lock lock(session_ptr->completion_resume_state->mutex);
                        session_ptr->completion_resume_state->agent = nullptr;
                    }

                    {
                        std::scoped_lock lock(*stream_sessions_mutex);
                        sessions_map->erase(captured_session_id);
                    }

                    sink.done();
                    return false;
                });
        } catch (const providers::ProviderError &e) {
            if (e.category() != providers::error_category::configuration && e.category() != providers::error_category::authentication) {
                throw;
            }
            send_error(res, 400, "provider_error", e.what());
        } catch (const std::exception &e) {
            send_error(res, 500, "internal_error", e.what());
        }
    }

    void handle_chat_abort(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.sessions == nullptr || ctx.sessions_mutex == nullptr) {
            send_error(res, 503, "sessions_unavailable", "session registry not wired");
            return;
        }
        const auto input = parse_body(req, res);
        if (!input.has_value()) {
            return;
        }
        if (!input->contains("session_id") || !(*input)["session_id"].is_string()) {
            send_error(res, 400, "invalid_request", "missing or invalid 'session_id' field");
            return;
        }

        const auto session_id = input->at("session_id").get<std::string>();
        std::scoped_lock lock(*ctx.sessions_mutex);
        const auto it = ctx.sessions->find(session_id);
        if (it == ctx.sessions->end()) {
            send_error(res, 404, "session_not_found", "session not found");
            return;
        }
        it->second->abort_requested = true;
        detail::cancel_pending_approval(*it->second);
        if (ctx.event_bus != nullptr) {
            ctx.event_bus->publish("chat.aborted", session_id, {{"session_id", session_id}});
        }
        send_json(res, {{"status", "abort_requested"}});
    }

    void handle_chat_approval(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.sessions == nullptr || ctx.sessions_mutex == nullptr) {
            send_error(res, 503, "sessions_unavailable", "session registry not wired");
            return;
        }
        const auto input = parse_body(req, res);
        if (!input.has_value()) {
            return;
        }

        if (!input->contains("session_id") || !(*input)["session_id"].is_string()) {
            send_error(res, 400, "invalid_request", "missing or invalid 'session_id' field");
            return;
        }
        if (!input->contains("request_id") || !(*input)["request_id"].is_string()) {
            send_error(res, 400, "invalid_request", "missing or invalid 'request_id' field");
            return;
        }
        if (!input->contains("approved") || !(*input)["approved"].is_boolean()) {
            send_error(res, 400, "invalid_request", "missing or invalid 'approved' field");
            return;
        }

        const auto session_id = input->at("session_id").get<std::string>();
        const auto request_id = input->at("request_id").get<std::string>();
        const bool approved = input->at("approved").get<bool>();

        std::shared_ptr<WebPendingApproval> pending;
        {
            std::scoped_lock lock(*ctx.sessions_mutex);
            const auto it = ctx.sessions->find(session_id);
            if (it == ctx.sessions->end()) {
                send_error(res, 404, "session_not_found", "session not found");
                return;
            }

            pending = it->second->pending_approval;
            if (pending == nullptr) {
                send_error(res, 404, "approval_not_found", "approval not found");
                return;
            }
        }

        std::scoped_lock approval_lock(pending->mutex);
        if (pending->request_id != request_id) {
            send_error(res, 404, "approval_not_found", "approval not found");
            return;
        }
        if (pending->resolved) {
            send_error(res, 409, "already_resolved", "approval already resolved");
            return;
        }

        pending->resolved = true;
        pending->approved = approved;
        pending->cancelled = !approved;
        pending->condition.notify_all();
        send_json(res, {{"status", approved ? "approved" : "denied"}});
    }

} // namespace orangutan::web
