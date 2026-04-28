#include "web/chat-session-controller.hpp"
#include "web/errors.hpp"
#include "web/event-bus.hpp"
#include "web/sse.hpp"
#include "web/web-route-internal.hpp"

#include "providers/provider.hpp"

namespace orangutan::web {

    namespace {

        void maybe_publish(const WebContext &ctx, std::string_view kind, const std::string &session_id, const std::string &agent_key,
                           nlohmann::json payload = nlohmann::json::object()) {
            if (ctx.event_bus == nullptr) {
                return;
            }
            payload["session_id"] = session_id;
            payload["agent_key"] = agent_key;
            ctx.event_bus->publish(kind, session_id, std::move(payload));
        }

        ChatSessionStreamCallbacks make_stream_callbacks(EventBus *event_bus, httplib::DataSink &sink, const std::string &session_id, const std::string &agent_key) {
            return ChatSessionStreamCallbacks{
                .session =
                    [&sink](const std::string &active_session_id) {
                        return write_sse(sink, "session", {{"session_id", active_session_id}});
                    },
                .text =
                    [event_bus, &sink, session_id, agent_key](const std::string &text) {
                        const bool wrote = write_sse(sink, "text", {{"text", text}});
                        if (event_bus != nullptr) {
                            event_bus->publish("chat.text", session_id, {{"session_id", session_id}, {"agent_key", agent_key}, {"text", text}});
                        }
                        return wrote;
                    },
                .thinking =
                    [&sink](const std::string &thinking) {
                        return write_sse(sink, "thinking", {{"thinking", thinking}});
                    },
                .tool_start =
                    [event_bus, &sink, session_id, agent_key](const ToolUse &call) {
                        const bool wrote = write_sse(sink, "tool_start", {{"id", call.id}, {"name", call.name}, {"input", call.input}});
                        if (event_bus != nullptr) {
                            event_bus->publish("chat.tool_start", session_id, {{"session_id", session_id}, {"agent_key", agent_key}, {"tool", call.name}, {"id", call.id}});
                        }
                        return wrote;
                    },
                .tool_end =
                    [event_bus, &sink, session_id, agent_key](const ToolUse &call, const ToolResult &result) {
                        const bool wrote = write_sse(sink, "tool_end", {{"id", call.id}, {"name", call.name}, {"content", result.content}, {"is_error", result.is_error}});
                        if (event_bus != nullptr) {
                            event_bus->publish("chat.tool_end", session_id,
                                                   {{"session_id", session_id}, {"agent_key", agent_key}, {"tool", call.name}, {"id", call.id}, {"is_error", result.is_error}});
                        }
                        return wrote;
                    },
                .done =
                    [event_bus, &sink, session_id, agent_key] {
                        const bool wrote = write_sse(sink, "done", nlohmann::json::object());
                        if (event_bus != nullptr) {
                            event_bus->publish("chat.done", session_id, {{"session_id", session_id}, {"agent_key", agent_key}});
                        }
                        return wrote;
                    },
                .error =
                    [event_bus, &sink, session_id, agent_key](std::string_view error) {
                        const auto error_text = std::string(error);
                        const bool wrote = write_sse(sink, "error", {{"error", error_text}});
                        if (event_bus != nullptr) {
                            event_bus->publish("chat.error", session_id, {{"session_id", session_id}, {"agent_key", agent_key}, {"error", error_text}});
                        }
                        return wrote;
                    },
                .complete =
                    [&sink] {
                        sink.done();
                    },
                .approval_event_emitter =
                    [&sink](std::string_view event_name, const nlohmann::json &payload) {
                        return write_sse(sink, event_name, payload);
                    },
                .stream_open =
                    [&sink] {
                        return sink.is_writable == nullptr || sink.is_writable();
                    },
            };
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
            if (const auto command_response = internal::handle_web_static_slash_command(message, agent_key, *ctx.config, ctx.session_store, existing_session, metadata, session_id);
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
            ChatSessionController controller(ctx);
            auto start_result = controller.start(ChatSessionStartRequest{
                .message = message,
                .agent_key = agent_key,
                .agent = *maybe_agent,
                .metadata = metadata,
                .session_id = session_id,
            });
            if (start_result.status == ChatSessionStartStatus::command_handled) {
                internal::send_web_command_stream(res, start_result.command_response);
                return;
            }
            if (start_result.status == ChatSessionStartStatus::session_active) {
                send_error(res, 409, "session_active", "session already active");
                return;
            }

            const auto active_session = std::move(start_result.active_session);
            const auto active_session_id = active_session->session_id();
            maybe_publish(ctx, "chat.session_started", active_session_id, agent_key, {{"preview", message.substr(0, 140)}});

            prepare_sse_response(res);
            res.set_chunked_content_provider("text/event-stream", [controller, active_session, event_bus = ctx.event_bus, agent_key](std::size_t /*offset*/, httplib::DataSink &sink) -> bool {
                controller.stream(active_session, make_stream_callbacks(event_bus, sink, active_session->session_id(), agent_key));
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
