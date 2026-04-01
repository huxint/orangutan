#pragma once

#include "bootstrap/identity.hpp"
#include "tools/registry/tool.hpp"
#include "config/config.hpp"

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

} // namespace orangutan

namespace orangutan::bootstrap {

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
        int thinking_budget = 0;
        Config::MemoryConfig memory;
        ToolPermissionSettings permissions;
        std::vector<std::string> allowed_child_agents;
        RuntimeIdentity identity;

        orangutan::MemoryStore *memory_store = nullptr;
        std::string *current_session_id = nullptr;
        orangutan::SubagentManager *subagent_manager = nullptr;
        base::origin runtime_origin = base::origin::cli;
        std::string raw_caller_id = "cli:local";
        orangutan::automation::Runtime *automation_runtime = nullptr;
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

        // This is a runtime bundle with intentionally exposed handles for assembly sites.
        // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
        std::unique_ptr<orangutan::Provider> provider;
        std::unique_ptr<orangutan::RuntimeMemory> memory;
        ToolRegistry &tools;
        ToolRuntimeContext &tool_context;
        std::unique_ptr<orangutan::McpManager> mcp_manager;
        std::string system_prompt;
        std::string skills_prompt;
        std::unique_ptr<orangutan::HookManager> hook_manager;
        std::unique_ptr<orangutan::AgentLoop> agent;
        // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

        friend AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input);
    };

    [[nodiscard]]
    AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input);

} // namespace orangutan::bootstrap
