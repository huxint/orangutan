#pragma once

#include "bootstrap/agent-runtime.hpp"
#include "web/context.hpp"

#include <httplib.h>

#include <chrono>
#include <functional>
#include <string>

namespace orangutan::web {

    struct WebCompletionResumeState;
    struct WebSessionState;

    // All v1 handlers share a uniform 3-argument signature: (context, request, response).
    // Context aggregates every backing service, so handlers are easy to register and test.

    void handle_list_sessions(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_get_session(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_delete_session(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_list_agent_sessions(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_get_agent_session(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_delete_agent_session(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);

    void handle_get_config(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_put_config(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);

    void handle_list_tools(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_list_agents(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_agent_graph(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_list_skills(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);

    void handle_list_automations(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_create_automation(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_get_automation(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_patch_automation(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_delete_automation(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_run_automation(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_pause_automation(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_resume_automation(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_list_automation_runs(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_list_automation_deliveries(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_ack_automation_delivery(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_clear_automation_deliveries(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);

    void handle_system_status(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_server_info(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);

    void handle_event_stream(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);

    void handle_chat(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_chat_approval(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);
    void handle_chat_abort(const WebContext &ctx, const httplib::Request &req, httplib::Response &res);

    namespace detail {

        using web_approval_event_emitter = std::function<bool(std::string_view event_name, const nlohmann::json &payload)>;

        [[nodiscard]]
        bootstrap::AgentRuntimeBundle build_web_runtime_bundle(const config::Config &config, const std::string &agent_key, memory::MemoryStore *memory_store,
                                                               std::string *current_session_id, automation::AutomationService *automation_service = nullptr,
                                                               automation::AutomationRuntime *automation_runtime = nullptr,
                                                               ApprovalCallback approval_callback = {},
                                                               const std::shared_ptr<WebCompletionResumeState> &completion_resume_state = {});

        [[nodiscard]]
        BackgroundCompletionResumeCallback make_web_completion_resume_callback(const std::weak_ptr<WebCompletionResumeState> &state);

        [[nodiscard]]
        bool await_web_approval(WebSessionState &session, std::mutex &sessions_mutex, const ToolUse &call, const PermissionDecision &decision,
                                const web_approval_event_emitter &event_emitter = {}, const std::function<bool()> &stream_open = {},
                                std::chrono::milliseconds timeout = std::chrono::minutes(2));

        void cancel_pending_approval(WebSessionState &session);

    } // namespace detail

} // namespace orangutan::web
