#include "features/web/web-routes.hpp"
#include "features/web/web-types.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/config/config.hpp"
#include "core/tools/tool.hpp"
#include "features/skills/skill-loader.hpp"
#include "core/providers/provider.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace orangutan::web {

void handle_list_sessions(const httplib::Request & /*req*/, httplib::Response &res, SessionStore *store) {
    if (store == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"session store not available"})", "application/json");
        return;
    }
    auto sessions = store->list_sessions();
    auto arr = nlohmann::json::array();
    for (const auto &s : sessions) {
        arr.push_back({
            {"id", s.id},
            {"created_at", s.created_at},
            {"model", s.model},
            {"message_count", s.message_count},
        });
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_get_session(const httplib::Request &req, httplib::Response &res, SessionStore *store) {
    if (store == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"session store not available"})", "application/json");
        return;
    }
    auto session_id = std::string(req.matches[1]);
    std::vector<Message> messages;
    try {
        messages = store->load(session_id);
    } catch (const std::runtime_error &) {
        res.status = 404;
        res.set_content(R"({"error":"session not found"})", "application/json");
        return;
    }
    auto arr = nlohmann::json::array();
    for (const auto &msg : messages) {
        arr.push_back(message_to_json(msg));
    }
    nlohmann::json body = {
        {"id", session_id},
        {"messages", arr},
    };
    res.set_content(body.dump(), "application/json");
}

void handle_delete_session(const httplib::Request &req, httplib::Response &res, SessionStore *store) {
    if (store == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"session store not available"})", "application/json");
        return;
    }
    auto session_id = std::string(req.matches[1]);
    store->remove(session_id);
    res.set_content(R"({"status":"deleted"})", "application/json");
}

void handle_get_config(const httplib::Request & /*req*/, httplib::Response &res, Config *config) {
    if (config == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"config not available"})", "application/json");
        return;
    }
    nlohmann::json body = {
        {"provider", config->provider},
        {"model", config->model},
        {"base_url", config->base_url},
        {"temperature", config->temperature},
        {"max_iterations", config->max_iterations},
        {"max_tokens", config->max_tokens},
        {"workspace", config->workspace},
        {"edit_mode", config->edit_mode},
        {"system_prompt", config->system_prompt},
        {"auto_save", config->auto_save},
        {"allowed_tools", config->allowed_tools},
        {"denied_tools", config->denied_tools},
        {"fallback_models", config->fallback_models},
        {"memory",
         {
             {"mirror_enabled", config->memory.mirror_enabled},
             {"mirror_file", config->memory.mirror_file},
             {"journal_dir", config->memory.journal_dir},
         }},
    };
    // Intentionally omit api_key for security
    res.set_content(body.dump(), "application/json");
}

void handle_put_config(const httplib::Request &req, httplib::Response &res, Config *config) {
    if (config == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"config not available"})", "application/json");
        return;
    }
    nlohmann::json input;
    try {
        input = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::parse_error &e) {
        res.status = 400;
        res.set_content(R"({"error":"invalid JSON"})", "application/json");
        return;
    }

    // Update mutable fields if present
    if (input.contains("model")) {
        config->model = input["model"].get<std::string>();
    }
    if (input.contains("provider")) {
        config->provider = input["provider"].get<std::string>();
    }
    if (input.contains("base_url")) {
        config->base_url = input["base_url"].get<std::string>();
    }
    if (input.contains("temperature")) {
        config->temperature = input["temperature"].get<double>();
    }
    if (input.contains("max_iterations")) {
        config->max_iterations = input["max_iterations"].get<int>();
    }
    if (input.contains("max_tokens")) {
        config->max_tokens = input["max_tokens"].get<int>();
    }
    if (input.contains("workspace")) {
        config->workspace = input["workspace"].get<std::string>();
    }
    if (input.contains("edit_mode")) {
        config->edit_mode = input["edit_mode"].get<std::string>();
    }
    if (input.contains("system_prompt")) {
        config->system_prompt = input["system_prompt"].get<std::string>();
    }
    if (input.contains("auto_save")) {
        config->auto_save = input["auto_save"].get<bool>();
    }

    auto config_path = expand_home_path("~/.orangutan/config.toml");
    config->save_to(config_path);

    res.set_content(R"({"status":"saved"})", "application/json");
}

void handle_list_tools(const httplib::Request & /*req*/, httplib::Response &res, ToolRegistry *registry) {
    if (registry == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"tool registry not available"})", "application/json");
        return;
    }
    auto defs = registry->definitions();
    auto arr = nlohmann::json::array();
    for (const auto &d : defs) {
        arr.push_back({
            {"name", d.name},
            {"description", d.description},
            {"source", "builtin"},
        });
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_list_agents(const httplib::Request & /*req*/, httplib::Response &res, Config *config) {
    if (config == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"config not available"})", "application/json");
        return;
    }
    auto arr = nlohmann::json::array();
    for (const auto &[key, agent] : config->agents) {
        arr.push_back({
            {"key", key},
            {"provider", agent.provider},
            {"model", agent.model},
            {"base_url", agent.base_url},
            {"system_prompt", agent.system_prompt},
            {"workspace", agent.workspace},
            {"edit_mode", agent.edit_mode},
        });
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_list_skills(const httplib::Request & /*req*/, httplib::Response &res, SkillLoader *loader) {
    if (loader == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"skill loader not available"})", "application/json");
        return;
    }
    auto arr = nlohmann::json::array();
    for (const auto &s : loader->active_skills()) {
        arr.push_back({
            {"name", s.name},
            {"description", s.description},
            {"tools", s.tools},
            {"source_path", s.source_path},
        });
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_system_status(const httplib::Request & /*req*/, httplib::Response &res, std::chrono::steady_clock::time_point start_time, std::mutex &sessions_mutex,
                          const std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions) {

    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

    size_t active_sessions = 0;
    {
        std::lock_guard lock(sessions_mutex);
        active_sessions = sessions.size();
    }

    nlohmann::json body = {
        {"uptime_seconds", uptime},         {"active_web_sessions", active_sessions}, {"provider_health", nlohmann::json::object()},
        {"cron", nlohmann::json::object()}, {"heartbeat", nlohmann::json::object()},
    };
    res.set_content(body.dump(), "application/json");
}

void handle_chat(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store, ToolRegistry * /*tool_registry*/, std::mutex &sessions_mutex,
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

    auto message = input.at("message").get<std::string>();
    auto session_id = input.value("session_id", std::string{});

    if (session_id.empty() && store != nullptr) {
        session_id = store->create_empty(config->model);
    }
    if (session_id.empty()) {
        session_id = "web-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    auto provider = create_provider(config->provider, config->api_key, config->model, config->base_url);
    auto tools = std::make_unique<ToolRegistry>();
    register_builtin_tools(*tools);

    auto session = std::make_unique<WebSessionState>();
    session->session_id = session_id;
    session->provider = std::move(provider);
    session->tools = std::move(tools);
    session->agent = std::make_unique<AgentLoop>(*session->provider, *session->tools, config->system_prompt);

    auto *abort_flag = &session->abort_requested;
    session->tools->set_execution_guard([abort_flag](const ToolUseBlock &call) -> std::optional<ToolResultBlock> {
        if (abort_flag->load()) {
            return ToolResultBlock{.tool_use_id = call.id, .content = "Operation aborted by user", .is_error = true};
        }
        return std::nullopt;
    });

    auto *agent_ptr = session->agent.get();
    auto *session_ptr = session.get();

    {
        std::lock_guard lock(sessions_mutex);
        sessions[session_id] = std::move(session);
    }

    auto captured_session_id = session_id;
    auto *store_ptr = store;

    res.set_chunked_content_provider(
        "text/event-stream", [agent_ptr, session_ptr, store_ptr, captured_session_id, message, &sessions_mutex, &sessions](size_t /*offset*/, httplib::DataSink &sink) -> bool {
            // Send session event
            auto session_event = nlohmann::json({{"session_id", captured_session_id}}).dump();
            auto sse_session = "event: session\ndata: " + session_event + "\n\n";
            sink.write(sse_session.c_str(), sse_session.size());

            session_ptr->running = true;
            try {
                agent_ptr->run(
                    message,
                    // StreamCallback
                    [&sink, session_ptr](const std::string &event_type, const nlohmann::json &data) {
                        if (session_ptr->abort_requested) {
                            return;
                        }
                        if (event_type == "text_delta") {
                            auto payload = nlohmann::json({{"text", data["text"]}}).dump();
                            auto sse = "event: text\ndata: " + payload + "\n\n";
                            sink.write(sse.c_str(), sse.size());
                        }
                    },
                    // ToolEventCallback
                    [&sink, session_ptr](const std::string &event_type, const ToolUseBlock &call, const ToolResultBlock *result) {
                        if (session_ptr->abort_requested) {
                            return;
                        }
                        nlohmann::json payload;
                        if (event_type == "tool_start") {
                            payload = {{"id", call.id}, {"name", call.name}, {"input", call.input}};
                            auto sse = "event: tool_start\ndata: " + payload.dump() + "\n\n";
                            sink.write(sse.c_str(), sse.size());
                        } else if (event_type == "tool_end" && result != nullptr) {
                            payload = {{"id", call.id}, {"name", call.name}, {"content", result->content}, {"is_error", result->is_error}};
                            auto sse = "event: tool_end\ndata: " + payload.dump() + "\n\n";
                            sink.write(sse.c_str(), sse.size());
                        }
                    });

                auto done_sse = std::string("event: done\ndata: {}\n\n");
                sink.write(done_sse.c_str(), done_sse.size());
            } catch (const std::exception &e) {
                auto err = nlohmann::json({{"error", e.what()}}).dump();
                auto sse = "event: error\ndata: " + err + "\n\n";
                sink.write(sse.c_str(), sse.size());
            }

            session_ptr->running = false;

            // Save session to store
            if (store_ptr != nullptr) {
                try {
                    store_ptr->update(captured_session_id, agent_ptr->history());
                } catch (const std::exception &e) {
                    spdlog::warn("Failed to save session {}: {}", captured_session_id, e.what());
                }
            }

            // Clean up session
            {
                std::lock_guard lock(sessions_mutex);
                sessions.erase(captured_session_id);
            }

            sink.done();
            return false;
        });
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

    auto session_id = input.at("session_id").get<std::string>();
    std::lock_guard lock(sessions_mutex);
    auto it = sessions.find(session_id);
    if (it != sessions.end()) {
        it->second->abort_requested = true;
        res.set_content(R"({"status":"abort_requested"})", "application/json");
    } else {
        res.status = 404;
        res.set_content(R"({"error":"session not found"})", "application/json");
    }
}
} // namespace orangutan::web
