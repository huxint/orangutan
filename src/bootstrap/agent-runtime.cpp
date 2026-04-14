#include "bootstrap/agent-runtime.hpp"

#include "bootstrap/memory-context.hpp"
#include "bootstrap/identity.hpp"
#include "coordinator/coordinator-prompt.hpp"
#include "permissions/permission-state.hpp"
#include "prompt/system-prompt-sections.hpp"
#include "providers/provider.hpp"
#include "agent/agent-loop.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/runtime-memory.hpp"
#include "skills/skill-loader.hpp"
#include "tools/runtime-loader/runtime-loader.hpp"
#include "tools/skill/skill-tool.hpp"

#include <cstdlib>
#include <utility>
#include <vector>

namespace orangutan::bootstrap {
    namespace {

        std::vector<std::string> resolve_hook_directories(const AgentRuntimeBuildInput &input) {
            std::vector<std::string> hook_dirs = input.hook_paths;
            if (!hook_dirs.empty()) {
                return hook_dirs;
            }

            if (const char *home = std::getenv("HOME"); home != nullptr) {
                hook_dirs.emplace_back(std::string(home) + "/.orangutan/hooks");
            }
            if (!input.workspace_root.empty()) {
                hook_dirs.push_back(workspace_hooks_root(input.workspace_root).string());
            }
            return hook_dirs;
        }

    } // namespace

    AgentRuntimeBundle::AgentRuntimeBundle()
    : tool_context_storage_(std::make_unique<ToolRuntimeContext>()),
      tools_storage_(std::make_unique<ToolRegistry>()),
      permissions_storage_(std::make_unique<ToolPermissionContext>()) {}

    AgentRuntimeBundle::~AgentRuntimeBundle() = default;

    AgentRuntimeBundle::AgentRuntimeBundle(AgentRuntimeBundle &&other) noexcept
    : tool_context_storage_(other.tool_context_storage_ ? std::move(other.tool_context_storage_) : std::make_unique<ToolRuntimeContext>()),
      tools_storage_(other.tools_storage_ ? std::move(other.tools_storage_) : std::make_unique<ToolRegistry>()),
      permissions_storage_(other.permissions_storage_ ? std::move(other.permissions_storage_) : std::make_unique<ToolPermissionContext>()),
      provider(std::move(other.provider)),
      memory(std::move(other.memory)),
      mcp_manager(std::move(other.mcp_manager)),
      skills_prompt(std::move(other.skills_prompt)),
      skill_loader(std::move(other.skill_loader)),
      hook_manager(std::move(other.hook_manager)),
      agent(std::move(other.agent)) {}

    ToolRegistry &AgentRuntimeBundle::tools() noexcept {
        return *tools_storage_;
    }

    const ToolRegistry &AgentRuntimeBundle::tools() const noexcept {
        return *tools_storage_;
    }

    ToolRuntimeContext &AgentRuntimeBundle::tool_context() noexcept {
        return *tool_context_storage_;
    }

    const ToolRuntimeContext &AgentRuntimeBundle::tool_context() const noexcept {
        return *tool_context_storage_;
    }

    const ToolPermissionContext &AgentRuntimeBundle::permissions() const noexcept {
        return *permissions_storage_;
    }

    void AgentRuntimeBundle::replace_permissions(ToolPermissionContext context) {
        *permissions_storage_ = std::move(context);
    }

    AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input) {
        AgentRuntimeBundle runtime;

        runtime.provider = std::make_unique<providers::ProviderSystem>();

        if (input.memory_store != nullptr) {
            runtime.memory = std::make_unique<RuntimeMemory>(*input.memory_store, make_runtime_memory_context(input.identity, input.memory));
        }

        runtime.tool_context() = ToolRuntimeContext{
            .runtime_key = input.identity.runtime_key,
            .agent_key = input.agent_key,
            .agent_name = input.agent_name.empty() ? input.agent_key : input.agent_name,
            .scope_key = input.identity.memory_scope,
            .team_id = input.team_id,
            .current_session_id = input.current_session_id,
            .coordinator_manager = input.coordinator_manager,
            .team_manager = input.team_manager,
            .mailbox = input.mailbox,
            .team_agents = input.team_agents,
            .is_child_run = input.is_child_run,
            .coordinator_mode = input.coordinator_mode,
            .runtime_origin = input.runtime_origin,
            .raw_caller_id = input.raw_caller_id,
            .automation_runtime = input.automation_runtime,
            .abort_checker = input.abort_checker,
            .approval_callback = input.approval_callback,
            .background_completion_runtime = input.background_completion_runtime,
        };

        *runtime.permissions_storage_ = input.permission_context;
        runtime.tool_context().permission_context = runtime.permissions_storage_.get();
        runtime.tool_context().permission_rule_mutator = [permission_context = runtime.tool_context().permission_context](PermissionRule rule) {
            if (permission_context == nullptr) {
                return;
            }
            *permission_context = add_rule(*permission_context, std::move(rule));
        };
        auto tool_bootstrap = register_runtime_tools(runtime.tools(), runtime.memory.get(), std::filesystem::path(input.identity.workspace), runtime.tool_context_storage_.get(),
                                                     input.custom_tools, input.mcp_servers, runtime.permissions_storage_.get(), input.edit_mode);
        runtime.mcp_manager = std::move(tool_bootstrap.mcp_manager);

        runtime.skill_loader = std::make_unique<SkillLoader>();
        runtime.skill_loader->set_workspace_root(std::filesystem::path{input.workspace_root});
        runtime.skill_loader->load_from_directories(resolve_skill_directories(input.skill_paths, std::filesystem::path{input.workspace_root}));
        std::string prompt_guidance;
        if (input.coordinator_mode) {
            prompt_guidance = coordinator::get_coordinator_system_prompt(input.team_agents);
        } else if (input.is_child_run) {
            const auto task_description = input.delegated_task_prompt.empty() ? std::string("Complete the delegated task and report the result.") : input.delegated_task_prompt;
            prompt_guidance = coordinator::get_worker_system_prompt_addendum(input.agent_key, task_description);
        } else if (!input.team_agents.empty()) {
            prompt_guidance = append_agent_prompt_guidance({}, input.team_agents, false);
        }

        runtime.skills_prompt = std::move(prompt_guidance);
        const auto skills_prompt = skills::render_skill_prompt_section(runtime.skill_loader->list(skills::skill_list_query{.include_inactive = false}));
        if (!runtime.skills_prompt.empty() && !skills_prompt.empty()) {
            runtime.skills_prompt += "\n\n";
        }
        runtime.skills_prompt += skills_prompt;
        if (!input.coordinator_mode) {
            tools::register_skill_tool(runtime.tools(), *runtime.skill_loader);
        }

        runtime.hook_manager = std::make_unique<HookManager>();
        runtime.hook_manager->load_from_directories(resolve_hook_directories(input));

        runtime.agent =
            std::make_unique<AgentLoop>(*runtime.provider, input.provider_route, runtime.tools(), runtime.memory.get(), runtime.skills_prompt, runtime.hook_manager.get(),
                                        runtime.skill_loader.get());
        runtime.agent->set_thinking_budget(input.thinking_budget);
        runtime.agent->set_environment_info(prompt::EnvironmentInfo{
            .workspace_root = input.workspace_root,
            .model_name = input.provider_route.primary.model,
            .agent_key = input.agent_key,
            .is_channel_mode = input.runtime_origin != base::origin::cli,
            .is_sandboxed = false,
        });
        return runtime;
    }

} // namespace orangutan::bootstrap
