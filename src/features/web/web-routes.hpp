#pragma once

#include "app/runtime/agent-runtime.hpp"
#include <httplib.h>

namespace orangutan {

class MemoryStore;
class SessionStore;
class SubagentManager;
class ToolRegistry;
class SkillLoader;
struct Config;
struct WebSessionState;
namespace automation {
class Runtime;
}

} // namespace orangutan

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace orangutan::web {

void handle_list_sessions(const httplib::Request &req, httplib::Response &res, SessionStore *store);
void handle_get_session(const httplib::Request &req, httplib::Response &res, SessionStore *store);
void handle_delete_session(const httplib::Request &req, httplib::Response &res, SessionStore *store);
void handle_list_agent_sessions(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store);
void handle_get_agent_session(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store);
void handle_delete_agent_session(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store);

void handle_get_config(const httplib::Request &req, httplib::Response &res, Config *config);
void handle_put_config(const httplib::Request &req, httplib::Response &res, Config *config, const std::string *config_save_path = nullptr);

void handle_list_tools(const httplib::Request &req, httplib::Response &res, ToolRegistry *registry);
void handle_list_agents(const httplib::Request &req, httplib::Response &res, Config *config);
void handle_list_skills(const httplib::Request &req, httplib::Response &res, SkillLoader *loader);
void handle_list_tasks(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);
void handle_list_heartbeats(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);
void handle_list_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);
void handle_ack_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);
void handle_clear_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime);

void handle_system_status(const httplib::Request &req, httplib::Response &res, std::chrono::steady_clock::time_point start_time, std::mutex &sessions_mutex,
                          const std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions, automation::Runtime *automation_runtime);

void handle_chat(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store, MemoryStore *memory_store, SubagentManager *subagent_manager,
                 ToolRegistry *tool_registry, automation::Runtime *automation_runtime, std::mutex &sessions_mutex,
                 std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions);
void handle_chat_approval(const httplib::Request &req, httplib::Response &res, std::mutex &sessions_mutex,
                          std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions);
void handle_chat_abort(const httplib::Request &req, httplib::Response &res, std::mutex &sessions_mutex,
                       std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions);

namespace detail {

using WebApprovalEventEmitter = std::function<bool(std::string_view event_name, const json &payload)>;

[[nodiscard]]
AgentRuntimeBundle build_web_runtime_bundle(const Config &config, const std::string &agent_key, MemoryStore *memory_store, std::string *current_session_id,
                                            SubagentManager *subagent_manager, automation::Runtime *automation_runtime = nullptr, ToolApprovalCallback approval_callback = {});

[[nodiscard]]
bool await_web_approval(WebSessionState &session, std::mutex &sessions_mutex, const ToolUseBlock &call, ToolSandboxMode sandbox_mode, const std::string &prompt_text,
                        WebApprovalEventEmitter event_emitter = {}, std::function<bool()> stream_open = {}, std::chrono::milliseconds timeout = std::chrono::minutes(2));

void cancel_pending_approval(WebSessionState &session);

} // namespace detail

} // namespace orangutan::web
