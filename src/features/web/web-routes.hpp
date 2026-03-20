#pragma once

#include <httplib.h>

namespace orangutan {

class SessionStore;
class ToolRegistry;
class SkillLoader;
struct Config;
struct WebSessionState;

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
void handle_put_config(const httplib::Request &req, httplib::Response &res, Config *config);

void handle_list_tools(const httplib::Request &req, httplib::Response &res, ToolRegistry *registry);
void handle_list_agents(const httplib::Request &req, httplib::Response &res, Config *config);
void handle_list_skills(const httplib::Request &req, httplib::Response &res, SkillLoader *loader);

void handle_system_status(const httplib::Request &req, httplib::Response &res, std::chrono::steady_clock::time_point start_time, std::mutex &sessions_mutex,
                          const std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions);

void handle_chat(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store, ToolRegistry *tool_registry, std::mutex &sessions_mutex,
                 std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions);
void handle_chat_abort(const httplib::Request &req, httplib::Response &res, std::mutex &sessions_mutex,
                       std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions);

} // namespace orangutan::web
