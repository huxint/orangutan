#pragma once

#include "app/runtime/identity.hpp"
#include "core/tools/tool.hpp"
#include "infra/config/config.hpp"

#include <memory>
#include <string>
#include <vector>

namespace orangutan {

class AgentLoop;
class HookManager;
class McpManager;
class MemoryStore;
class Provider;
class RuntimeMemory;
class SubagentManager;
namespace automation {
class Runtime;
}

struct AgentRuntimeBuildInput {
    std::string provider_name;
    std::string api_key;
    std::string model;
    std::vector<std::string> fallback_models;
    std::string base_url;
    std::string agent_key;
    std::string system_prompt;
    std::string workspace_root;
    std::string edit_mode = "hashline";
    Config::MemoryConfig memory;
    ToolPermissionSettings permissions;
    std::vector<std::string> allowed_child_agents;
    RuntimeIdentity identity;

    MemoryStore *memory_store = nullptr;
    std::string *current_session_id = nullptr;
    SubagentManager *subagent_manager = nullptr;
    SubagentRuntimeOrigin runtime_origin = SubagentRuntimeOrigin::cli;
    std::string raw_caller_id = "cli:local";
    automation::Runtime *automation_runtime = nullptr;
    bool is_child_run = false;
    ToolApprovalCallback approval_callback;

    std::vector<Config::ScriptToolConfig> custom_tools;
    std::vector<Config::McpServerConfig> mcp_servers;
    std::vector<std::string> skill_paths;
    std::vector<std::string> hook_paths;
    std::shared_ptr<const BackgroundCompletionRuntimeBindings> background_completion_runtime;
};

struct AgentRuntimeBundle {
private:
    std::unique_ptr<ToolRuntimeContext> tool_context_storage_;
    std::unique_ptr<ToolRegistry> tools_storage_;
    std::unique_ptr<ToolPermissionSettings> permissions_storage_;

public:
    AgentRuntimeBundle();
    ~AgentRuntimeBundle();

    AgentRuntimeBundle(AgentRuntimeBundle &&other) noexcept;
    AgentRuntimeBundle &operator=(AgentRuntimeBundle &&other) = delete;

    AgentRuntimeBundle(const AgentRuntimeBundle &) = delete;
    AgentRuntimeBundle &operator=(const AgentRuntimeBundle &) = delete;

    std::unique_ptr<Provider> provider;
    std::unique_ptr<RuntimeMemory> memory;
    ToolRegistry &tools;
    ToolRuntimeContext &tool_context;
    std::unique_ptr<McpManager> mcp_manager;
    std::string system_prompt;
    std::string skills_prompt;
    std::unique_ptr<HookManager> hook_manager;
    std::unique_ptr<AgentLoop> agent;

    friend AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input);
};

[[nodiscard]]
AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input);

} // namespace orangutan
