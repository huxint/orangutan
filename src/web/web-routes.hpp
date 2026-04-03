#pragma once

#include "bootstrap/agent-runtime.hpp"
#include <httplib.h>

namespace orangutan::automation {
    class Runtime;
}

namespace orangutan::config {
    struct Config;
}

namespace orangutan::memory {
    class MemoryStore;
}

namespace orangutan::skills {
    class SkillLoader;
}

namespace orangutan::storage {
    class SessionStore;
}

namespace orangutan::tools {
    class ToolRegistry;
}

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace orangutan::web {

    struct WebCompletionResumeState;
    struct WebSessionState;

    void handle_list_sessions(const httplib::Request &req, httplib::Response &res, storage::SessionStore *store);
    void handle_get_session(const httplib::Request &req, httplib::Response &res, storage::SessionStore *store);
    void handle_delete_session(const httplib::Request &req, httplib::Response &res, storage::SessionStore *store);
    void handle_list_agent_sessions(const httplib::Request &req, httplib::Response &res, config::Config *config, storage::SessionStore *store);
    void handle_get_agent_session(const httplib::Request &req, httplib::Response &res, config::Config *config, storage::SessionStore *store);
    void handle_delete_agent_session(const httplib::Request &req, httplib::Response &res, config::Config *config, storage::SessionStore *store);

    void handle_get_config(const httplib::Request &req, httplib::Response &res, config::Config *config);
    void handle_put_config(const httplib::Request &req, httplib::Response &res, config::Config *config, const std::filesystem::path *config_save_path = nullptr);

    void handle_list_tools(const httplib::Request &req, httplib::Response &res, tools::ToolRegistry *registry);
    void handle_list_agents(const httplib::Request &req, httplib::Response &res, config::Config *config);
    void handle_list_skills(const httplib::Request &req, httplib::Response &res, skills::SkillLoader *loader);
    void handle_list_tasks(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);
    void handle_list_heartbeats(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);
    void handle_list_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);
    void handle_ack_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);
    void handle_clear_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);

    void handle_system_status(const httplib::Request &req, httplib::Response &res, std::chrono::steady_clock::time_point start_time, std::mutex &sessions_mutex,
                              const std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions, automation::Runtime *automation_runtime);

    void handle_chat(const httplib::Request &req, httplib::Response &res, config::Config *config, storage::SessionStore *store, memory::MemoryStore *memory_store,
                     tools::ToolRegistry *tool_registry, automation::Runtime *automation_runtime, std::mutex &sessions_mutex,
                     std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions);
    void handle_chat_approval(const httplib::Request &req, httplib::Response &res, std::mutex &sessions_mutex,
                              std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions);
    void handle_chat_abort(const httplib::Request &req, httplib::Response &res, std::mutex &sessions_mutex,
                           std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions);

    namespace detail {

        using web_approval_event_emitter = std::function<bool(std::string_view event_name, const nlohmann::json &payload)>;

        [[nodiscard]]
        bootstrap::AgentRuntimeBundle build_web_runtime_bundle(const config::Config &config, const std::string &agent_key, memory::MemoryStore *memory_store,
                                                               std::string *current_session_id, automation::Runtime *automation_runtime = nullptr,
                                                               ToolApprovalCallback approval_callback = {},
                                                               const std::shared_ptr<WebCompletionResumeState> &completion_resume_state = {});

        [[nodiscard]]
        BackgroundCompletionResumeCallback make_web_completion_resume_callback(const std::weak_ptr<WebCompletionResumeState> &state);

        [[nodiscard]]
        bool await_web_approval(WebSessionState &session, std::mutex &sessions_mutex, const ToolUse &call, ToolSandboxMode sandbox_mode, const std::string &prompt_text,
                                const web_approval_event_emitter &event_emitter = {}, const std::function<bool()> &stream_open = {},
                                std::chrono::milliseconds timeout = std::chrono::minutes(2));

        void cancel_pending_approval(WebSessionState &session);

    } // namespace detail

} // namespace orangutan::web
