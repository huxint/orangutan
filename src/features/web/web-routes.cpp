#include "features/web/web-routes.hpp"

#include "app/bootstrap.hpp"
#include "app/runtime/agent-runtime.hpp"
#include "app/runtime/identity.hpp"
#include "core/providers/provider.hpp"
#include "core/tools/permissions.hpp"
#include "features/agent/agent-loop.hpp"
#include "features/web/web-types.hpp"
#include "features/memory/memory.hpp"
#include "features/skills/skill-loader.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "infra/config/config.hpp"
#include "infra/storage/session-store.hpp"
#include "core/tools/tool.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace orangutan::web {

namespace {

bool session_is_read_only(const SessionInfo &session) {
    return session.origin_kind == "channel";
}

SessionMetadata make_web_session_metadata(const std::string &agent_key, const AgentConfig &agent) {
    return SessionMetadata{
        .model = agent.model,
        .scope_key = "agent:" + agent_key + "|web",
        .agent_key = agent_key,
        .origin_kind = "web",
        .origin_ref = "web:local",
    };
}

std::optional<AgentConfig> find_effective_agent(const Config *config, const std::string &agent_key) {
    if (config == nullptr) {
        return std::nullopt;
    }

    const auto effective_agents = app::detail::build_effective_agents(*config);
    if (const auto it = effective_agents.find(agent_key); it != effective_agents.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::string resolve_agent_api_key(const Config &config, const AgentConfig &agent) {
    Config agent_cfg_wrapper = config;
    agent_cfg_wrapper.api_key = agent.api_key;
    return app::detail::resolve_api_key("", agent_cfg_wrapper);
}

std::string resolve_agent_workspace(const AgentConfig &agent, const std::string &agent_key) {
    try {
        return orangutan::resolve_workspace_root(agent.workspace);
    } catch (const std::exception &e) {
        throw std::runtime_error("failed to resolve workspace for agent '" + agent_key + "': " + e.what());
    }
}

RuntimeIdentity derive_web_identity(const std::string &workspace_root, const std::string &agent_key) {
    RuntimeIdentity identity{
        .workspace = workspace_root,
        .runtime_key = "agent:" + agent_key + "|web:local",
        .memory_scope = "agent:" + agent_key + "|web",
    };
    return identity;
}

ToolApprovalCallback default_web_approval_callback() {
    return [](const ToolUseBlock & /*call*/, const std::string & /*prompt_text*/) {
        // Task 3 keeps approval callback parity in runtime context.
        // Task 4 will replace this with request/response coordination.
        return false;
    };
}

AgentRuntimeBundle build_web_runtime_bundle_impl(const Config &config, const AgentConfig &agent, const std::string &agent_key, MemoryStore *memory_store,
                                                 std::string *current_session_id, SubagentManager *subagent_manager, ToolApprovalCallback approval_callback) {
    const auto workspace_root = resolve_agent_workspace(agent, agent_key);
    const auto api_key = resolve_agent_api_key(config, agent);
    if (api_key.empty()) {
        throw MissingApiKeyError("missing API key for agent '" + agent_key + "'");
    }

    auto effective_approval_callback = std::move(approval_callback);
    if (!effective_approval_callback) {
        effective_approval_callback = default_web_approval_callback();
    }

    AgentRuntimeBuildInput input{
        .provider_name = agent.provider,
        .api_key = api_key,
        .model = agent.model,
        .fallback_models = agent.fallback_models,
        .base_url = agent.base_url,
        .agent_key = agent_key,
        .system_prompt = agent.system_prompt,
        .workspace_root = workspace_root,
        .edit_mode = agent.edit_mode,
        .memory = config.memory,
        .permissions = agent.permissions,
        .allowed_child_agents = agent.subagents,
        .identity = derive_web_identity(workspace_root, agent_key),
        .memory_store = memory_store,
        .current_session_id = current_session_id,
        .subagent_manager = subagent_manager,
        .runtime_origin = SubagentRuntimeOrigin::web,
        .raw_caller_id = "web:local",
        .approval_callback = effective_approval_callback,
        .custom_tools = config.custom_tools,
        .mcp_servers = config.mcp_servers,
        .skill_paths = config.skill_paths,
        .hook_paths = config.hook_paths,
    };
    return build_agent_runtime(input);
}

nlohmann::json session_to_json(const SessionInfo &session) {
    return {
        {"id", session.id},
        {"created_at", session.created_at},
        {"model", session.model},
        {"scope_key", session.scope_key},
        {"agent_key", session.agent_key},
        {"origin_kind", session.origin_kind},
        {"origin_ref", session.origin_ref},
        {"message_count", session.message_count},
        {"read_only", session_is_read_only(session)},
    };
}

std::optional<SessionInfo> find_agent_session(SessionStore *store, const std::string &agent_key, const std::string &session_id) {
    if (store == nullptr) {
        return std::nullopt;
    }

    const auto sessions = store->list_sessions_for_agent(agent_key);
    for (const auto &session : sessions) {
        if (session.id == session_id) {
            return session;
        }
    }
    return std::nullopt;
}

} // namespace

AgentRuntimeBundle detail::build_web_runtime_bundle(const Config &config, const std::string &agent_key, MemoryStore *memory_store, std::string *current_session_id,
                                                    SubagentManager *subagent_manager, ToolApprovalCallback approval_callback) {
    const auto maybe_agent = find_effective_agent(&config, agent_key);
    if (!maybe_agent.has_value()) {
        throw std::runtime_error("agent not found");
    }
    return build_web_runtime_bundle_impl(config, *maybe_agent, agent_key, memory_store, current_session_id, subagent_manager, std::move(approval_callback));
}

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

void handle_list_agent_sessions(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store) {
    if (config == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"config not available"})", "application/json");
        return;
    }
    if (store == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"session store not available"})", "application/json");
        return;
    }

    const auto agent_key = std::string(req.matches[1]);
    if (!find_effective_agent(config, agent_key).has_value()) {
        res.status = 404;
        res.set_content(R"({"error":"agent not found"})", "application/json");
        return;
    }

    auto arr = nlohmann::json::array();
    for (const auto &session : store->list_sessions_for_agent(agent_key)) {
        arr.push_back(session_to_json(session));
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_get_agent_session(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store) {
    if (config == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"config not available"})", "application/json");
        return;
    }
    if (store == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"session store not available"})", "application/json");
        return;
    }

    const auto agent_key = std::string(req.matches[1]);
    const auto session_id = std::string(req.matches[2]);
    if (!find_effective_agent(config, agent_key).has_value()) {
        res.status = 404;
        res.set_content(R"({"error":"agent not found"})", "application/json");
        return;
    }

    const auto session = find_agent_session(store, agent_key, session_id);
    if (!session.has_value()) {
        res.status = 404;
        res.set_content(R"({"error":"session not found"})", "application/json");
        return;
    }

    try {
        auto arr = nlohmann::json::array();
        for (const auto &message : store->load(session_id)) {
            arr.push_back(message_to_json(message));
        }

        auto body = session_to_json(*session);
        body["messages"] = arr;
        res.set_content(body.dump(), "application/json");
    } catch (const std::runtime_error &) {
        res.status = 404;
        res.set_content(R"({"error":"session not found"})", "application/json");
    }
}

void handle_delete_agent_session(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store) {
    if (config == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"config not available"})", "application/json");
        return;
    }
    if (store == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"session store not available"})", "application/json");
        return;
    }

    const auto agent_key = std::string(req.matches[1]);
    const auto session_id = std::string(req.matches[2]);
    if (!find_effective_agent(config, agent_key).has_value()) {
        res.status = 404;
        res.set_content(R"({"error":"agent not found"})", "application/json");
        return;
    }
    if (!store->session_belongs_to_agent(session_id, agent_key)) {
        res.status = 404;
        res.set_content(R"({"error":"session not found"})", "application/json");
        return;
    }

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

void handle_put_config(const httplib::Request &req, httplib::Response &res, Config *config, const std::string *config_save_path) {
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

    const auto config_path = (config_save_path != nullptr && !config_save_path->empty()) ? *config_save_path : default_orangutan_config_path();
    if (config_path.empty()) {
        res.status = 500;
        res.set_content(R"({"error":"unable to resolve config save path"})", "application/json");
        return;
    }
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
    for (const auto &[key, agent] : app::detail::build_effective_agents(*config)) {
        arr.push_back({
            {"key", key},
            {"provider", agent.provider},
            {"model", agent.model},
            {"base_url", agent.base_url},
            {"system_prompt", agent.system_prompt},
            {"workspace", agent.workspace},
            {"edit_mode", agent.edit_mode},
            {"subagents", agent.subagents},
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

void handle_chat(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store, MemoryStore *memory_store, SubagentManager *subagent_manager,
                 ToolRegistry * /*tool_registry*/, std::mutex &sessions_mutex, std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions) {
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
    const auto maybe_agent = find_effective_agent(config, agent_key);
    if (!maybe_agent.has_value()) {
        res.status = 404;
        res.set_content(R"({"error":"agent not found"})", "application/json");
        return;
    }
    const auto metadata = make_web_session_metadata(agent_key, *maybe_agent);
    std::string session_id;
    if (input.contains("session_id") && !input["session_id"].is_null()) {
        if (!input["session_id"].is_string()) {
            res.status = 400;
            res.set_content(R"({"error":"invalid 'session_id' field"})", "application/json");
            return;
        }
        session_id = input.at("session_id").get<std::string>();
    }

    if (!session_id.empty()) {
        if (store == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"session store not available"})", "application/json");
            return;
        }
        const auto existing_session = find_agent_session(store, agent_key, session_id);
        if (!existing_session.has_value()) {
            res.status = 404;
            res.set_content(R"({"error":"session not found"})", "application/json");
            return;
        }
        if (session_is_read_only(*existing_session)) {
            res.status = 409;
            res.set_content(R"({"error":"channel sessions are read-only in web chat"})", "application/json");
            return;
        }
    }

    try {
        if (session_id.empty() && store != nullptr) {
            session_id = store->create_empty(metadata);
        }
        if (session_id.empty()) {
            session_id = "web-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        }

        auto session = std::make_unique<WebSessionState>();
        session->session_id = session_id;
        session->runtime = std::make_unique<AgentRuntimeBundle>(detail::build_web_runtime_bundle(*config, agent_key, memory_store, &session->session_id, subagent_manager));
        if (session->agent() == nullptr) {
            throw std::runtime_error("failed to initialize web runtime agent");
        }

        if (!session_id.empty() && store != nullptr) {
            session->agent()->set_history(store->load(session_id));
        }

        auto *abort_flag = &session->abort_requested;
        const auto permissions = maybe_agent->permissions;
        auto *tool_context = &session->runtime->tool_context;
        if (auto *tools = session->tools(); tools != nullptr) {
            tools->set_execution_guard([abort_flag, permissions, tool_context](const ToolUseBlock &call) -> std::optional<ToolResultBlock> {
                if (abort_flag->load()) {
                    return ToolResultBlock{.tool_use_id = call.id, .content = "Operation aborted by user", .is_error = true};
                }
                const auto &approval_callback = tool_context != nullptr ? tool_context->approval_callback : ToolApprovalCallback{};
                return evaluate_tool_permission(call, permissions, approval_callback);
            });
        }

        auto *agent_ptr = session->agent();
        auto *session_ptr = session.get();

        {
            std::lock_guard lock(sessions_mutex);
            sessions[session_id] = std::move(session);
        }

        auto captured_session_id = session_id;
        auto *store_ptr = store;
        auto captured_metadata = metadata;

        res.set_chunked_content_provider(
            "text/event-stream",
            [agent_ptr, session_ptr, store_ptr, captured_session_id, captured_metadata, message, &sessions_mutex, &sessions](size_t /*offset*/, httplib::DataSink &sink) -> bool {
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
                            if (event_type == "tool_started" || event_type == "tool_start") {
                                payload = {{"id", call.id}, {"name", call.name}, {"input", call.input}};
                                auto sse = "event: tool_start\ndata: " + payload.dump() + "\n\n";
                                sink.write(sse.c_str(), sse.size());
                            } else if ((event_type == "tool_finished" || event_type == "tool_end") && result != nullptr) {
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
                        store_ptr->update(captured_session_id, agent_ptr->history(), captured_metadata);
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
    } catch (const MissingApiKeyError &e) {
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
