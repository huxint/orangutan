#include "features/web/web-routes.hpp"

#include "app/single-shot.hpp"
#include "features/automation/runtime.hpp"
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
                                                 std::string *current_session_id, SubagentManager *subagent_manager, automation::Runtime *automation_runtime,
                                                 ToolApprovalCallback approval_callback, const std::shared_ptr<WebCompletionResumeState> &completion_resume_state) {
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
        .automation_runtime = automation_runtime,
        .approval_callback = effective_approval_callback,
        .custom_tools = config.custom_tools,
        .mcp_servers = config.mcp_servers,
        .skill_paths = config.skill_paths,
        .hook_paths = config.hook_paths,
        .background_completion_runtime =
            automation_runtime != nullptr
                ? make_background_completion_runtime_bindings(
                      [automation_runtime](const automation::InboxItem &item) {
                          (void)automation_runtime->store().insert_inbox(item);
                      },
                      completion_resume_state != nullptr ? detail::make_web_completion_resume_callback(completion_resume_state) : BackgroundCompletionResumeCallback{})
                : nullptr,
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

std::optional<std::string> extract_approval_command(const ToolUseBlock &call) {
    if (!call.input.is_object()) {
        return std::nullopt;
    }
    const auto it = call.input.find("command");
    if (it == call.input.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

std::string make_approval_request_id() {
    static std::atomic<uint64_t> next_request_id{1};
    return "approval-" + std::to_string(next_request_id.fetch_add(1, std::memory_order_relaxed));
}

json approval_payload(const WebPendingApproval &approval) {
    json payload = {
        {"request_id", approval.request_id},
        {"tool", approval.tool},
        {"sandbox_mode", approval.sandbox_mode},
        {"prompt", approval.prompt},
    };
    if (approval.command.has_value()) {
        payload["command"] = *approval.command;
    }
    return payload;
}

std::string resolve_agent_key_param(const httplib::Request &req) {
    if (req.has_param("agent_key")) {
        return req.get_param_value("agent_key");
    }
    return "default";
}

json unix_time_to_json(const std::optional<std::int64_t> &value) {
    if (!value.has_value()) {
        return nullptr;
    }
    return *value;
}

json task_to_json(const automation::TaskSpec &task) {
    return {
        {"id", task.id},
        {"agent_key", task.agent_key},
        {"name", task.name},
        {"enabled", task.enabled},
        {"schedule_kind", automation::task_schedule_kind_to_string(task.schedule.kind)},
        {"schedule", task.schedule.value},
        {"prompt", task.prompt},
        {"notes", task.notes},
        {"delivery", automation::delivery_policy_to_json(task.delivery)},
        {"last_run_at", unix_time_to_json(task.last_run_at)},
        {"last_status", task.last_status},
    };
}

json heartbeat_to_json(const automation::HeartbeatSpec &heartbeat) {
    return {
        {"id", heartbeat.id},
        {"agent_key", heartbeat.agent_key},
        {"name", heartbeat.name},
        {"enabled", heartbeat.enabled},
        {"paused", heartbeat.paused},
        {"every_seconds", heartbeat.every_seconds},
        {"jitter_seconds", heartbeat.jitter_seconds},
        {"active_hours", automation::active_hours_to_json(heartbeat.active_hours)},
        {"prompt", heartbeat.prompt},
        {"notes", heartbeat.notes},
        {"delivery", automation::delivery_policy_to_json(heartbeat.delivery)},
        {"next_due_at", unix_time_to_json(heartbeat.next_due_at)},
        {"last_run_at", unix_time_to_json(heartbeat.last_run_at)},
        {"last_status", heartbeat.last_status},
    };
}

json inbox_item_to_json(const automation::InboxItem &item) {
    return {
        {"id", item.id},         {"agent_key", item.agent_key}, {"source_kind", item.source_kind}, {"source_run_id", item.source_run_id},
        {"title", item.title},   {"body", item.body},           {"created_at", item.created_at},   {"acked_at", unix_time_to_json(item.acked_at)},
        {"status", item.status},
    };
}

void resolve_pending_approval(WebPendingApproval &approval, bool approved, bool cancelled) {
    std::lock_guard lock(approval.mutex);
    if (approval.resolved) {
        return;
    }
    approval.resolved = true;
    approval.approved = approved;
    approval.cancelled = cancelled;
    approval.condition.notify_all();
}

} // namespace

AgentRuntimeBundle detail::build_web_runtime_bundle(const Config &config, const std::string &agent_key, MemoryStore *memory_store, std::string *current_session_id,
                                                    SubagentManager *subagent_manager, automation::Runtime *automation_runtime, ToolApprovalCallback approval_callback,
                                                    std::shared_ptr<WebCompletionResumeState> completion_resume_state) {
    const auto maybe_agent = find_effective_agent(&config, agent_key);
    if (!maybe_agent.has_value()) {
        throw std::runtime_error("agent not found");
    }
    return build_web_runtime_bundle_impl(config, *maybe_agent, agent_key, memory_store, current_session_id, subagent_manager, automation_runtime, std::move(approval_callback),
                                         completion_resume_state);
}

BackgroundCompletionResumeCallback detail::make_web_completion_resume_callback(std::weak_ptr<WebCompletionResumeState> weak_state) {
    return [weak_state](const std::string &message) -> std::optional<std::string> {
        const auto state = weak_state.lock();
        if (!state) {
            return "web session is no longer live";
        }

        std::scoped_lock lock(state->mutex);
        if (state->agent == nullptr) {
            return "web session is no longer live";
        }

        return orangutan::app::run_completion_resume_message(*state->agent, message, state->agent_key, state->automation_runtime, {}, true);
    };
}

bool detail::await_web_approval(WebSessionState &session, std::mutex &sessions_mutex, const ToolUseBlock &call, ToolSandboxMode sandbox_mode, const std::string &prompt_text,
                                WebApprovalEventEmitter event_emitter, std::function<bool()> stream_open, std::chrono::milliseconds timeout) {
    auto approval = std::make_shared<WebPendingApproval>();
    approval->request_id = make_approval_request_id();
    approval->tool = call.name;
    approval->command = extract_approval_command(call);
    approval->sandbox_mode = to_string(sandbox_mode);
    approval->prompt = prompt_text;

    {
        std::lock_guard sessions_lock(sessions_mutex);
        if (session.abort_requested.load()) {
            return false;
        }
        if (session.pending_approval != nullptr && !session.pending_approval->resolved) {
            return false;
        }
        session.pending_approval = approval;
    }

    if (event_emitter && !event_emitter("approval_request", approval_payload(*approval))) {
        resolve_pending_approval(*approval, false, true);
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock approval_lock(approval->mutex);
    while (!approval->resolved) {
        if (stream_open && !stream_open()) {
            approval->resolved = true;
            approval->approved = false;
            approval->cancelled = true;
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            approval->resolved = true;
            approval->approved = false;
            approval->cancelled = true;
            break;
        }

        const auto wait_for = std::min(std::chrono::milliseconds(100), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        approval->condition.wait_for(approval_lock, wait_for, [&approval] {
            return approval->resolved;
        });
    }
    const bool approved = approval->approved;
    approval_lock.unlock();

    {
        std::lock_guard sessions_lock(sessions_mutex);
        if (session.pending_approval == approval) {
            session.pending_approval.reset();
        }
    }

    return approved;
}

void detail::cancel_pending_approval(WebSessionState &session) {
    if (session.pending_approval == nullptr) {
        return;
    }
    resolve_pending_approval(*session.pending_approval, false, true);
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

void handle_list_tasks(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
    if (automation_runtime == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"automation runtime not available"})", "application/json");
        return;
    }

    auto arr = nlohmann::json::array();
    for (const auto &task : automation_runtime->list_tasks(resolve_agent_key_param(req))) {
        arr.push_back(task_to_json(task));
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_list_heartbeats(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
    if (automation_runtime == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"automation runtime not available"})", "application/json");
        return;
    }

    auto arr = nlohmann::json::array();
    for (const auto &heartbeat : automation_runtime->list_heartbeats(resolve_agent_key_param(req))) {
        arr.push_back(heartbeat_to_json(heartbeat));
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_list_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
    if (automation_runtime == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"automation runtime not available"})", "application/json");
        return;
    }

    auto arr = nlohmann::json::array();
    for (const auto &item : automation_runtime->list_inbox(resolve_agent_key_param(req))) {
        arr.push_back(inbox_item_to_json(item));
    }
    res.set_content(arr.dump(), "application/json");
}

void handle_ack_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
    if (automation_runtime == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"automation runtime not available"})", "application/json");
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

    if (!input.contains("id") || !input["id"].is_string()) {
        res.status = 400;
        res.set_content(R"({"error":"missing or invalid 'id' field"})", "application/json");
        return;
    }

    const auto agent_key = input.value("agent_key", std::string("default"));
    if (!automation_runtime->ack_inbox(agent_key, input["id"].get<std::string>())) {
        res.status = 404;
        res.set_content(R"({"error":"inbox item not found"})", "application/json");
        return;
    }
    res.set_content(R"({"status":"acknowledged"})", "application/json");
}

void handle_clear_inbox(const httplib::Request &req, httplib::Response &res, automation::Runtime *automation_runtime) {
    if (automation_runtime == nullptr) {
        res.status = 503;
        res.set_content(R"({"error":"automation runtime not available"})", "application/json");
        return;
    }

    automation_runtime->clear_inbox(resolve_agent_key_param(req));
    res.set_content(R"({"status":"cleared"})", "application/json");
}

void handle_system_status(const httplib::Request & /*req*/, httplib::Response &res, std::chrono::steady_clock::time_point start_time, std::mutex &sessions_mutex,
                          const std::unordered_map<std::string, std::unique_ptr<WebSessionState>> &sessions, automation::Runtime *automation_runtime) {

    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

    size_t active_sessions = 0;
    {
        std::lock_guard lock(sessions_mutex);
        active_sessions = sessions.size();
    }

    nlohmann::json body = {
        {"uptime_seconds", uptime},
        {"active_web_sessions", active_sessions},
        {"provider_health", nlohmann::json::object()},
    };
    if (automation_runtime == nullptr) {
        body["automation"] = nullptr;
    } else {
        const auto all_tasks = automation_runtime->list_tasks({});
        const auto all_heartbeats = automation_runtime->list_heartbeats({});
        body["automation"] = {
            {"task_count", all_tasks.size()},
            {"heartbeat_count", all_heartbeats.size()},
        };
    }
    res.set_content(body.dump(), "application/json");
}

void handle_chat(const httplib::Request &req, httplib::Response &res, Config *config, SessionStore *store, MemoryStore *memory_store, SubagentManager *subagent_manager,
                 ToolRegistry * /*tool_registry*/, automation::Runtime *automation_runtime, std::mutex &sessions_mutex,
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
        session->completion_resume_state = std::make_shared<WebCompletionResumeState>();
        session->completion_resume_state->agent_key = agent_key;
        session->completion_resume_state->automation_runtime = automation_runtime;
        auto *session_ptr = session.get();
        auto approval_event_emitter = std::make_shared<detail::WebApprovalEventEmitter>();
        auto approval_stream_open = std::make_shared<std::function<bool()>>();
        session->runtime = std::make_unique<AgentRuntimeBundle>(detail::build_web_runtime_bundle(
            *config, agent_key, memory_store, &session->session_id, subagent_manager, automation_runtime,
            [session_ptr, &sessions_mutex, sandbox_mode = maybe_agent->permissions.sandbox_mode, approval_event_emitter, approval_stream_open](const ToolUseBlock &call,
                                                                                                                                               const std::string &prompt_text) {
                return detail::await_web_approval(*session_ptr, sessions_mutex, call, sandbox_mode, prompt_text,
                                                  approval_event_emitter != nullptr ? *approval_event_emitter : detail::WebApprovalEventEmitter{},
                                                  approval_stream_open != nullptr ? *approval_stream_open : std::function<bool()>{});
            },
            session->completion_resume_state));
        if (session->agent() == nullptr) {
            throw std::runtime_error("failed to initialize web runtime agent");
        }
        session->completion_resume_state->agent = session->agent();

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

        {
            std::lock_guard lock(sessions_mutex);
            sessions[session_id] = std::move(session);
        }

        auto captured_session_id = session_id;
        auto *store_ptr = store;
        auto captured_metadata = metadata;

        res.set_chunked_content_provider("text/event-stream",
                                         [agent_ptr, session_ptr, store_ptr, captured_session_id, captured_metadata, message, approval_event_emitter, approval_stream_open,
                                          agent_key, automation_runtime, &sessions_mutex, &sessions](size_t /*offset*/, httplib::DataSink &sink) -> bool {
                                             if (approval_event_emitter != nullptr) {
                                                 *approval_event_emitter = [&sink](std::string_view event_name, const json &payload) {
                                                     const auto sse = "event: " + std::string(event_name) + "\ndata: " + payload.dump() + "\n\n";
                                                     return sink.write(sse.c_str(), sse.size());
                                                 };
                                             }
                                             if (approval_stream_open != nullptr) {
                                                 *approval_stream_open = [&sink]() {
                                                     return sink.is_writable == nullptr || sink.is_writable();
                                                 };
                                             }

                                             // Send session event
                                             auto session_event = nlohmann::json({{"session_id", captured_session_id}}).dump();
                                             auto sse_session = "event: session\ndata: " + session_event + "\n\n";
                                             sink.write(sse_session.c_str(), sse_session.size());

                                             session_ptr->running = true;
                                             try {
                                                 automation::with_agent_execution_lease(automation_runtime, agent_key, [&] {
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
                                                 });

                                                 auto done_sse = std::string("event: done\ndata: {}\n\n");
                                                 sink.write(done_sse.c_str(), done_sse.size());
                                             } catch (const std::exception &e) {
                                                 auto err = nlohmann::json({{"error", e.what()}}).dump();
                                                 auto sse = "event: error\ndata: " + err + "\n\n";
                                                 sink.write(sse.c_str(), sse.size());
                                             }

                                             session_ptr->running = false;
                                             detail::cancel_pending_approval(*session_ptr);
                                             if (approval_event_emitter != nullptr) {
                                                 *approval_event_emitter = {};
                                             }
                                             if (approval_stream_open != nullptr) {
                                                 *approval_stream_open = {};
                                             }
                                             if (session_ptr->completion_resume_state != nullptr) {
                                                 std::scoped_lock lock(session_ptr->completion_resume_state->mutex);
                                                 // Once the request-scoped web session is tearing down, later background completions
                                                 // must downgrade to inbox-only failure notes instead of touching destroyed state.
                                                 session_ptr->completion_resume_state->agent = nullptr;
                                             }

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
        std::lock_guard lock(sessions_mutex);
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

    std::lock_guard approval_lock(pending->mutex);
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
