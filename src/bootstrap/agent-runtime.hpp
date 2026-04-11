#pragma once

#include "bootstrap/identity.hpp"
#include "config/config.hpp"
#include "permissions/permission-types.hpp"
#include "providers/provider.hpp"
#include "skills/skill-loader.hpp"
#include "tools/registry/tool.hpp"

#include <memory>
#include <string>
#include <vector>

namespace orangutan::agent {
    class AgentLoop;
}

namespace orangutan::automation {
    class Runtime;
}

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::memory {
    class MemoryStore;
    class RuntimeMemory;
} // namespace orangutan::memory

namespace orangutan::providers {
    class Provider;
}

namespace orangutan::coordinator {
    class CoordinatorManager;
}

namespace orangutan::swarm {
    class AgentMailbox;
    class TeamManager;
} // namespace orangutan::swarm

namespace orangutan::tools {
    class McpManager;
}

namespace orangutan::bootstrap {

    struct AgentRuntimeBuildInput {
        providers::ProviderEndpoint primary_endpoint;
        std::vector<providers::ProviderEndpoint> fallback_endpoints;
        std::string agent_key;
        std::string agent_name;
        std::string workspace_root;
        std::string edit_mode = "hashline";
        int thinking_budget = 0;
        Config::MemoryConfig memory;
        ToolPermissionContext permission_context;
        std::vector<std::string> team_agents;
        std::string team_id;
        RuntimeIdentity identity;

        memory::MemoryStore *memory_store = nullptr;
        std::string *current_session_id = nullptr;
        coordinator::CoordinatorManager *coordinator_manager = nullptr;
        swarm::TeamManager *team_manager = nullptr;
        swarm::AgentMailbox *mailbox = nullptr;
        base::origin runtime_origin = base::origin::cli;
        std::string raw_caller_id = "cli:local";
        automation::Runtime *automation_runtime = nullptr;
        bool is_child_run = false;
        bool coordinator_mode = false;
        RuntimeAbortChecker abort_checker;
        ApprovalCallback approval_callback;
        std::string delegated_task_prompt;

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
        std::unique_ptr<ToolPermissionContext> permissions_storage_;

    public:
        AgentRuntimeBundle();
        ~AgentRuntimeBundle();

        AgentRuntimeBundle(AgentRuntimeBundle &&other) noexcept;
        AgentRuntimeBundle &operator=(AgentRuntimeBundle &&other) = delete;

        AgentRuntimeBundle(const AgentRuntimeBundle &) = delete;
        AgentRuntimeBundle &operator=(const AgentRuntimeBundle &) = delete;

        // This is a runtime bundle with intentionally exposed handles for assembly sites.
        // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
        std::unique_ptr<providers::Provider> provider;
        std::unique_ptr<memory::RuntimeMemory> memory;
        std::unique_ptr<tools::McpManager> mcp_manager;
        std::string skills_prompt;
        std::unique_ptr<skills::SkillLoader> skill_loader;
        std::unique_ptr<hooks::HookManager> hook_manager;
        std::unique_ptr<agent::AgentLoop> agent;
        // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

        [[nodiscard]]
        ToolRegistry &tools() noexcept;
        [[nodiscard]]
        const ToolRegistry &tools() const noexcept;

        [[nodiscard]]
        ToolRuntimeContext &tool_context() noexcept;
        [[nodiscard]]
        const ToolRuntimeContext &tool_context() const noexcept;
        [[nodiscard]]
        const ToolPermissionContext &permissions() const noexcept;

        void replace_permissions(ToolPermissionContext context);

        friend AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input);
    };

    [[nodiscard]]
    AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input);

} // namespace orangutan::bootstrap
