#include "web/web-route-internal.hpp"

#include "agent/agent-loop.hpp"
#include "providers/provider.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"

#include <spdlog/spdlog.h>

namespace orangutan::web {

    namespace bootstrap = orangutan::bootstrap;

    void handle_chat(const httplib::Request &req, httplib::Response &res, config::Config *config, storage::SessionStore *store, memory::MemoryStore *memory_store,
                     tools::ToolRegistry * /*tool_registry*/, automation::Runtime *automation_runtime,
                     std::mutex &sessions_mutex,
                     std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions) {
        if (config == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"config not available"})", "application/json");
            return;
        }

        nlohmann::json input;
        try {
            input = nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::parse_error &) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }

        if (!input.contains("message") || !input["message"].is_string()) {
            res.status = 400;
            res.set_content(R"({"error":"missing or invalid 'message' field"})", "application/json");
            return;
        }
        if (!input.contains("agent_key") || !input["agent_key"].is_string()) {
            res.status = 400;
            res.set_content(R"({"error":"missing or invalid 'agent_key' field"})", "application/json");
            return;
        }

        auto message = input.at("message").get<std::string>();
        const auto agent_key = input.at("agent_key").get<std::string>();
        const auto maybe_agent = internal::find_effective_agent(config, agent_key);
        if (!maybe_agent.has_value()) {
            res.status = 404;
            res.set_content(R"({"error":"agent not found"})", "application/json");
            return;
        }
        const auto metadata = internal::make_web_session_metadata(agent_key, *maybe_agent);
        std::string session_id;
        if (input.contains("session_id") && !input["session_id"].is_null()) {
            if (!input["session_id"].is_string()) {
                res.status = 400;
                res.set_content(R"({"error":"invalid 'session_id' field"})", "application/json");
                return;
            }
            session_id = input.at("session_id").get<std::string>();
        }

        std::optional<storage::SessionInfo> existing_session;
        if (!session_id.empty()) {
            if (store == nullptr) {
                res.status = 503;
                res.set_content(R"({"error":"session store not available"})", "application/json");
                return;
            }
            existing_session = internal::find_agent_session(store, agent_key, session_id);
            if (!existing_session.has_value()) {
                res.status = 404;
                res.set_content(R"({"error":"session not found"})", "application/json");
                return;
            }
        }

        if (message.starts_with('/')) {
            if (const auto command_response = internal::handle_web_static_slash_command(message, agent_key, *config, store, existing_session, metadata, session_id);
                command_response.handled) {
                internal::send_web_command_stream(res, command_response);
                return;
            }
        }

        if (existing_session.has_value() && internal::session_is_read_only(*existing_session)) {
            res.status = 409;
            res.set_content(R"({"error":"channel sessions are read-only in web chat"})", "application/json");
            return;
        }

        if (!session_id.empty()) {
            std::scoped_lock lock(sessions_mutex);
            if (sessions.contains(session_id)) {
                res.status = 409;
                res.set_content(R"({"error":"session already active"})", "application/json");
                return;
            }
        }

        try {
            auto session = std::make_unique<WebSessionState>();
            session->session_id = session_id;
            session->completion_resume_state = std::make_shared<WebCompletionResumeState>();
            session->completion_resume_state->agent_key = agent_key;
            session->completion_resume_state->automation_runtime = automation_runtime;
            auto *session_ptr = session.get();
            auto approval_event_emitter = std::make_shared<detail::web_approval_event_emitter>();
            auto approval_stream_open = std::make_shared<std::function<bool()>>();
            session->runtime = std::make_unique<bootstrap::AgentRuntimeBundle>(detail::build_web_runtime_bundle(
                *config, agent_key, memory_store, &session->session_id, automation_runtime,
                [session_ptr, &sessions_mutex, approval_event_emitter, approval_stream_open](const ToolUse &call,
                                                                                              const PermissionDecision &decision) {
                    return detail::await_web_approval(*session_ptr, sessions_mutex, call, decision,
                                                      approval_event_emitter != nullptr ? *approval_event_emitter : detail::web_approval_event_emitter{},
                                                      approval_stream_open != nullptr ? *approval_stream_open : std::function<bool()>{});
                },
                session->completion_resume_state));
            if (session->agent() == nullptr) {
                throw std::runtime_error("failed to initialize web runtime agent");
            }

            if (!session->session_id.empty() && store != nullptr) {
                session->agent()->set_history(store->load(session->session_id));
            }

            if (message.starts_with('/')) {
                if (const auto command_response =
                        internal::handle_web_runtime_slash_command(message, agent_key, *maybe_agent, store, metadata, *session->runtime, session->session_id);
                    command_response.handled) {
                    internal::send_web_command_stream(res, command_response);
                    return;
                }
            }

            if (session->session_id.empty() && store != nullptr) {
                session->session_id = store->create_empty(metadata);
            }
            if (session->session_id.empty()) {
                session->session_id = "web-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            }

            auto *abort_flag = &session->abort_requested;
            auto *tool_context = &session->runtime->tool_context;
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
                std::scoped_lock lock(sessions_mutex);
                const auto [inserted_it, inserted] = sessions.try_emplace(active_session_id, std::move(session));
                if (!inserted) {
                    res.status = 409;
                    res.set_content(R"({"error":"session already active"})", "application/json");
                    return;
                }
                session_ptr = inserted_it->second.get();
            }

            auto *store_ptr = store;

            res.set_chunked_content_provider(
                "text/event-stream",
                [agent_ptr, session_ptr, store_ptr, captured_session_id = active_session_id, captured_metadata = metadata, message, approval_event_emitter, approval_stream_open,
                 agent_key, automation_runtime, &sessions_mutex, &sessions](std::size_t /*offset*/, httplib::DataSink &sink) -> bool {
                    if (approval_event_emitter != nullptr) {
                        *approval_event_emitter = [&sink](std::string_view event_name, const nlohmann::json &payload) {
                            const auto sse = "event: " + static_cast<std::string>(event_name) + "\ndata: " + payload.dump() + "\n\n";
                            return sink.write(sse.c_str(), sse.size());
                        };
                    }
                    if (approval_stream_open != nullptr) {
                        *approval_stream_open = [&sink]() {
                            return sink.is_writable == nullptr || sink.is_writable();
                        };
                    }

                    internal::write_sse_event(sink, "session", {{"session_id", captured_session_id}});

                    session_ptr->running = true;
                    try {
                        automation::with_agent_execution_lease(automation_runtime, agent_key, [&] {
                            agent_ptr->run(
                                message,
                                [&sink, session_ptr](const std::string &event_type, const nlohmann::json &data) {
                                    if (session_ptr->abort_requested) {
                                        return;
                                    }
                                    if (event_type == "text_delta") {
                                        internal::write_sse_event(sink, "text", {{"text", data["text"]}});
                                    } else if (event_type == "thinking_delta") {
                                        internal::write_sse_event(sink, "thinking", {{"thinking", data["thinking"]}});
                                    }
                                },
                                [&sink, session_ptr](const std::string &event_type, const ToolUse &call, const ToolResult *result) {
                                    if (session_ptr->abort_requested) {
                                        return;
                                    }
                                    if (event_type == "tool_started" || event_type == "tool_start") {
                                        internal::write_sse_event(sink, "tool_start", {{"id", call.id}, {"name", call.name}, {"input", call.input}});
                                    } else if ((event_type == "tool_finished" || event_type == "tool_end") && result != nullptr) {
                                        internal::write_sse_event(sink, "tool_end",
                                                                  {{"id", call.id}, {"name", call.name}, {"content", result->content}, {"is_error", result->is_error}});
                                    }
                                });
                        });

                        internal::write_sse_event(sink, "done", nlohmann::json::object());
                    } catch (const std::exception &e) {
                        internal::write_sse_event(sink, "error", {{"error", e.what()}});
                    }

                    session_ptr->running = false;
                    detail::cancel_pending_approval(*session_ptr);
                    if (approval_event_emitter != nullptr) {
                        *approval_event_emitter = {};
                    }
                    if (approval_stream_open != nullptr) {
                        *approval_stream_open = {};
                    }

                    if (store_ptr != nullptr) {
                        try {
                            store_ptr->update(captured_session_id, agent_ptr->history(), captured_metadata);
                        } catch (const std::exception &e) {
                            spdlog::warn("Failed to save session {}: {}", captured_session_id, e.what());
                        }
                    }

                    if (session_ptr->completion_resume_state != nullptr) {
                        std::scoped_lock lock(session_ptr->completion_resume_state->mutex);
                        session_ptr->completion_resume_state->agent = nullptr;
                    }

                    {
                        std::scoped_lock lock(sessions_mutex);
                        sessions.erase(captured_session_id);
                    }

                    sink.done();
                    return false;
                });
        } catch (const providers::MissingApiKeyError &e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        } catch (const std::exception &e) {
            res.status = 500;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    }

    void handle_chat_abort(const httplib::Request &req, httplib::Response &res, std::mutex &sessions_mutex,
                           std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions) {
        nlohmann::json input;
        try {
            input = nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::parse_error &) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }

        if (!input.contains("session_id") || !input["session_id"].is_string()) {
            res.status = 400;
            res.set_content(R"({"error":"missing or invalid 'session_id' field"})", "application/json");
            return;
        }

        const auto session_id = input.at("session_id").get<std::string>();
        std::scoped_lock lock(sessions_mutex);
        const auto it = sessions.find(session_id);
        if (it != sessions.end()) {
            it->second->abort_requested = true;
            detail::cancel_pending_approval(*it->second);
            res.set_content(R"({"status":"abort_requested"})", "application/json");
        } else {
            res.status = 404;
            res.set_content(R"({"error":"session not found"})", "application/json");
        }
    }

    void handle_chat_approval(const httplib::Request &req, httplib::Response &res, std::mutex &sessions_mutex,
                              std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions) {
        nlohmann::json input;
        try {
            input = nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::parse_error &) {
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }

        if (!input.contains("session_id") || !input["session_id"].is_string()) {
            res.status = 400;
            res.set_content(R"({"error":"missing or invalid 'session_id' field"})", "application/json");
            return;
        }
        if (!input.contains("request_id") || !input["request_id"].is_string()) {
            res.status = 400;
            res.set_content(R"({"error":"missing or invalid 'request_id' field"})", "application/json");
            return;
        }
        if (!input.contains("approved") || !input["approved"].is_boolean()) {
            res.status = 400;
            res.set_content(R"({"error":"missing or invalid 'approved' field"})", "application/json");
            return;
        }

        const auto session_id = input.at("session_id").get<std::string>();
        const auto request_id = input.at("request_id").get<std::string>();
        const bool approved = input.at("approved").get<bool>();

        std::shared_ptr<WebPendingApproval> pending;
        {
            std::scoped_lock lock(sessions_mutex);
            const auto it = sessions.find(session_id);
            if (it == sessions.end()) {
                res.status = 404;
                res.set_content(R"({"error":"session not found"})", "application/json");
                return;
            }

            pending = it->second->pending_approval;
            if (pending == nullptr) {
                res.status = 404;
                res.set_content(R"({"error":"approval not found"})", "application/json");
                return;
            }
        }

        std::scoped_lock approval_lock(pending->mutex);
        if (pending->request_id != request_id) {
            res.status = 404;
            res.set_content(R"({"error":"approval not found"})", "application/json");
            return;
        }
        if (pending->resolved) {
            res.status = 409;
            res.set_content(R"({"error":"approval already resolved"})", "application/json");
            return;
        }

        pending->resolved = true;
        pending->approved = approved;
        pending->cancelled = !approved;
        pending->condition.notify_all();
        res.status = 200;
        res.set_content((approved ? R"({"status":"approved"})" : R"({"status":"denied"})"), "application/json");
    }

} // namespace orangutan::web
