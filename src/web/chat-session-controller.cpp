#include "web/chat-session-controller.hpp"

#include "agent/agent-loop.hpp"
#include "automation/runtime.hpp"
#include "bootstrap/agent-runtime.hpp"
#include "tools/registry/tool-context.hpp"
#include "utils/scope-exit.hpp"
#include "web/web-route-internal.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

namespace orangutan::web {

    namespace {

        void set_completion_resume_agent(WebSessionState &session, agent::AgentLoop *agent) {
            if (session.completion_resume_state == nullptr) {
                return;
            }

            std::scoped_lock lock(session.completion_resume_state->mutex);
            session.completion_resume_state->agent = agent;
        }

        void persist_session(storage::SessionStore *store, WebSessionState &session, const std::string &session_id, const storage::SessionMetadata &metadata) {
            if (store == nullptr || session.agent() == nullptr) {
                return;
            }

            try {
                const auto &history = session.agent()->history();
                if (history.size() > session.persisted_message_count) {
                    store->append(session_id, history, session.persisted_message_count, metadata);
                } else {
                    store->update(session_id, history, metadata);
                }
                session.persisted_message_count = history.size();
            } catch (const std::exception &e) {
                spdlog::warn("failed to save session {}: {}", session_id, e.what());
            }
        }

        template <typename Fn>
        bool emit_if_present(const Fn &fn) {
            if (fn == nullptr) {
                return true;
            }
            return fn();
        }

    } // namespace

    ActiveChatSession::ActiveChatSession(std::mutex *sessions_mutex, std::unordered_map<std::string, std::unique_ptr<WebSessionState>> *sessions, std::string session_id,
                                         std::string agent_key, std::string message, storage::SessionMetadata metadata, WebSessionState *session,
                                         std::shared_ptr<detail::web_approval_event_emitter> approval_event_emitter,
                                         std::shared_ptr<std::function<bool()>> approval_stream_open)
    : sessions_mutex_(sessions_mutex),
      sessions_(sessions),
      session_id_(std::move(session_id)),
      agent_key_(std::move(agent_key)),
      message_(std::move(message)),
      metadata_(std::move(metadata)),
      session_(session),
      approval_event_emitter_(std::move(approval_event_emitter)),
      approval_stream_open_(std::move(approval_stream_open)) {}

    ActiveChatSession::~ActiveChatSession() {
        cleanup();
    }

    const std::string &ActiveChatSession::session_id() const noexcept {
        return session_id_;
    }

    const std::string &ActiveChatSession::agent_key() const noexcept {
        return agent_key_;
    }

    const std::string &ActiveChatSession::message() const noexcept {
        return message_;
    }

    const storage::SessionMetadata &ActiveChatSession::metadata() const noexcept {
        return metadata_;
    }

    WebSessionState *ActiveChatSession::session() const noexcept {
        return session_;
    }

    agent::AgentLoop *ActiveChatSession::agent() const noexcept {
        return session_ != nullptr ? session_->agent() : nullptr;
    }

    void ActiveChatSession::cleanup() {
        if (cleaned_up_) {
            return;
        }
        cleaned_up_ = true;

        if (approval_event_emitter_ != nullptr) {
            *approval_event_emitter_ = {};
        }
        if (approval_stream_open_ != nullptr) {
            *approval_stream_open_ = {};
        }

        const auto cleanup_session = [&](WebCompletionResumeState *locked_resume_state) {
            if (sessions_mutex_ == nullptr || sessions_ == nullptr || session_id_.empty()) {
                return;
            }

            std::scoped_lock lock(*sessions_mutex_);
            const auto it = sessions_->find(session_id_);
            if (it != sessions_->end() && it->second != nullptr) {
                auto &session = *it->second;
                session.running = false;
                if (locked_resume_state != nullptr && session.completion_resume_state.get() == locked_resume_state) {
                    locked_resume_state->agent = nullptr;
                }
                detail::cancel_pending_approval(session);
                detached_session_ = std::move(it->second);
                sessions_->erase(it);
            }
        };

        const auto resume_state = session_ != nullptr ? session_->completion_resume_state : nullptr;
        if (resume_state != nullptr) {
            std::scoped_lock resume_lock(resume_state->mutex);
            cleanup_session(resume_state.get());
        } else {
            cleanup_session(nullptr);
        }
        session_ = nullptr;
    }

    ChatSessionController::ChatSessionController(const WebContext &ctx)
    : config_(ctx.config),
      session_store_(ctx.session_store),
      memory_store_(ctx.memory_store),
      automation_runtime_(ctx.automation_runtime),
      sessions_mutex_(ctx.sessions_mutex),
      sessions_(ctx.sessions) {}

    ChatSessionStartResult ChatSessionController::start(const ChatSessionStartRequest &request) const {
        if (config_ == nullptr) {
            throw std::runtime_error("config not available");
        }
        if (sessions_mutex_ == nullptr || sessions_ == nullptr) {
            throw std::runtime_error("session registry not wired");
        }

        auto session = std::make_unique<WebSessionState>();
        session->session_id = request.session_id;
        session->completion_resume_state = std::make_shared<WebCompletionResumeState>();
        session->completion_resume_state->agent_key = request.agent_key;
        session->completion_resume_state->automation_runtime = automation_runtime_;

        auto *session_ptr = session.get();
        auto approval_event_emitter = std::make_shared<detail::web_approval_event_emitter>();
        auto approval_stream_open = std::make_shared<std::function<bool()>>();
        auto *automation_service = automation_runtime_ != nullptr ? &automation_runtime_->service() : nullptr;
        std::mutex *sessions_mutex = sessions_mutex_;
        session->runtime = std::make_unique<bootstrap::AgentRuntimeBundle>(detail::build_web_runtime_bundle(
            *config_, request.agent_key, memory_store_, &session->session_id, automation_service, automation_runtime_,
            [session_ptr, sessions_mutex, approval_event_emitter, approval_stream_open](const ToolUse &call, const PermissionDecision &decision) {
                return detail::await_web_approval(*session_ptr, *sessions_mutex, call, decision,
                                                  approval_event_emitter != nullptr ? *approval_event_emitter : detail::web_approval_event_emitter{},
                                                  approval_stream_open != nullptr ? *approval_stream_open : std::function<bool()>{});
            },
            session->completion_resume_state));
        if (session->agent() == nullptr) {
            throw std::runtime_error("failed to initialize web runtime agent");
        }

        if (!session->session_id.empty() && session_store_ != nullptr) {
            session->agent()->set_history(session_store_->load(session->session_id));
            session->persisted_message_count = session->agent()->history().size();
        }

        if (request.message.starts_with('/')) {
            auto command_response = internal::handle_web_runtime_slash_command(request.message, request.agent_key, request.agent, session_store_, request.metadata,
                                                                                *session->runtime, session->session_id);
            if (command_response.handled) {
                return ChatSessionStartResult{.status = ChatSessionStartStatus::command_handled, .command_response = std::move(command_response)};
            }
        }

        if (session->session_id.empty() && session_store_ != nullptr) {
            session->session_id = session_store_->create_empty(request.metadata);
        }
        if (session->session_id.empty()) {
            session->session_id = "web-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        }

        auto *abort_flag = &session->abort_requested;
        session->runtime->tool_context().abort_checker = [abort_flag] {
            return abort_flag->load();
        };
        auto *agent_ptr = session->agent();
        set_completion_resume_agent(*session, agent_ptr);

        const auto active_session_id = session->session_id;
        {
            std::scoped_lock lock(*sessions_mutex_);
            const auto [inserted_it, inserted] = sessions_->try_emplace(active_session_id, std::move(session));
            if (!inserted) {
                return ChatSessionStartResult{.status = ChatSessionStartStatus::session_active};
            }
            session_ptr = inserted_it->second.get();
        }

        auto inserted_session_cleanup = utils::scope_exit([&] {
            std::scoped_lock lock(*sessions_mutex_);
            sessions_->erase(active_session_id);
        });
        auto active_session = std::shared_ptr<ActiveChatSession>(
            new ActiveChatSession(sessions_mutex_, sessions_, active_session_id, request.agent_key, request.message, request.metadata, session_ptr, approval_event_emitter,
                                  approval_stream_open));
        inserted_session_cleanup.release();

        return ChatSessionStartResult{.status = ChatSessionStartStatus::stream_ready, .active_session = std::move(active_session)};
    }

    void ChatSessionController::stream(const std::shared_ptr<ActiveChatSession> &active_session, ChatSessionStreamCallbacks callbacks) const {
        if (active_session == nullptr || active_session->session() == nullptr || active_session->agent() == nullptr) {
            if (callbacks.error != nullptr) {
                static_cast<void>(callbacks.error("web session is no longer live"));
            }
            if (callbacks.complete != nullptr) {
                callbacks.complete();
            }
            return;
        }

        auto *session = active_session->session();
        auto *agent_ptr = active_session->agent();
        auto cleanup_guard = utils::scope_exit([&active_session] {
            active_session->cleanup();
        });
        session->running = true;
        if (active_session->approval_event_emitter_ != nullptr) {
            *active_session->approval_event_emitter_ = std::move(callbacks.approval_event_emitter);
        }
        if (active_session->approval_stream_open_ != nullptr) {
            *active_session->approval_stream_open_ = std::move(callbacks.stream_open);
        }

        auto detached_session_cleanup = utils::scope_exit([&active_session] {
            active_session->detached_session_.reset();
        });
        automation::with_agent_execution_lease(automation_runtime_, active_session->agent_key(), [&] {
            auto leased_cleanup = utils::scope_exit([&] {
                persist_session(session_store_, *session, active_session->session_id(), active_session->metadata());
                active_session->cleanup();
                cleanup_guard.release();
            });

            static_cast<void>(callbacks.session != nullptr ? callbacks.session(active_session->session_id()) : true);

            try {
                agent_ptr->run(
                    active_session->message(),
                    [session, &callbacks](const ProviderEvent &event) {
                        if (session->abort_requested.load()) {
                            return;
                        }
                        if (const auto *text = std::get_if<TextDelta>(&event)) {
                            static_cast<void>(callbacks.text != nullptr ? callbacks.text(text->text) : true);
                        } else if (const auto *thinking = std::get_if<ThinkingDelta>(&event)) {
                            static_cast<void>(callbacks.thinking != nullptr ? callbacks.thinking(thinking->thinking) : true);
                        }
                    },
                    [session, &callbacks](const std::string &event_type, const ToolUse &call, const ToolResult *result) {
                        if (session->abort_requested.load()) {
                            return;
                        }
                        if (event_type == "tool_started" || event_type == "tool_start") {
                            static_cast<void>(callbacks.tool_start != nullptr ? callbacks.tool_start(call) : true);
                        } else if ((event_type == "tool_finished" || event_type == "tool_end") && result != nullptr) {
                            static_cast<void>(callbacks.tool_end != nullptr ? callbacks.tool_end(call, *result) : true);
                        }
                    });

                static_cast<void>(emit_if_present(callbacks.done));
            } catch (const std::exception &e) {
                static_cast<void>(callbacks.error != nullptr ? callbacks.error(e.what()) : true);
            }
        });
        active_session->detached_session_.reset();
        detached_session_cleanup.release();
        if (callbacks.complete != nullptr) {
            callbacks.complete();
        }
    }

} // namespace orangutan::web
