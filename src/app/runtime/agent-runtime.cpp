#include "app/runtime/agent-runtime.hpp"

#include "app/runtime/memory-context.hpp"
#include "providers/provider.hpp"
#include "agent/agent-loop.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/runtime-memory.hpp"
#include "skills/skill-loader.hpp"
#include "tools/runtime-loader/runtime-loader.hpp"

#include <cstdlib>
#include <utility>
#include <vector>

namespace orangutan {
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
                hook_dirs.push_back(input.workspace_root + "/.orangutan/hooks");
            }
            return hook_dirs;
        }

    } // namespace

    AgentRuntimeBundle::AgentRuntimeBundle()
    : tool_context_storage_(std::make_unique<ToolRuntimeContext>()),
      tools_storage_(std::make_unique<ToolRegistry>()),
      permissions_storage_(std::make_unique<ToolPermissionSettings>()),
      tools(*tools_storage_),
      tool_context(*tool_context_storage_) {}

    AgentRuntimeBundle::~AgentRuntimeBundle() = default;

    AgentRuntimeBundle::AgentRuntimeBundle(AgentRuntimeBundle &&other) noexcept
    : tool_context_storage_(other.tool_context_storage_ ? std::move(other.tool_context_storage_) : std::make_unique<ToolRuntimeContext>()),
      tools_storage_(other.tools_storage_ ? std::move(other.tools_storage_) : std::make_unique<ToolRegistry>()),
      permissions_storage_(other.permissions_storage_ ? std::move(other.permissions_storage_) : std::make_unique<ToolPermissionSettings>()),
      provider(std::move(other.provider)),
      memory(std::move(other.memory)),
      tools(*tools_storage_),
      tool_context(*tool_context_storage_),
      mcp_manager(std::move(other.mcp_manager)),
      system_prompt(std::move(other.system_prompt)),
      skills_prompt(std::move(other.skills_prompt)),
      hook_manager(std::move(other.hook_manager)),
      agent(std::move(other.agent)) {}

    AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input) {
        AgentRuntimeBundle runtime;

        runtime.provider = create_provider_with_fallbacks(input.provider_name, input.api_key, input.model, input.base_url, input.fallback_models);

        if (input.memory_store != nullptr) {
            runtime.memory = std::make_unique<RuntimeMemory>(*input.memory_store, make_runtime_memory_context(input.identity, input.memory));
        }

        runtime.tool_context = ToolRuntimeContext{
            .runtime_key = input.identity.runtime_key,
            .agent_key = input.agent_key,
            .scope_key = input.identity.memory_scope,
            .current_session_id = input.current_session_id,
            .allowed_child_agents = input.allowed_child_agents,
            .is_child_run = input.is_child_run,
            .subagent_manager = input.subagent_manager,
            .runtime_origin = input.runtime_origin,
            .raw_caller_id = input.raw_caller_id,
            .automation_runtime = input.automation_runtime,
            .approval_callback = input.approval_callback,
            .background_completion_runtime = input.background_completion_runtime,
        };

        *runtime.permissions_storage_ = input.permissions;
        auto tool_bootstrap = register_runtime_tools(runtime.tools, runtime.memory.get(), input.identity.workspace, runtime.tool_context_storage_.get(), input.custom_tools,
                                                     input.mcp_servers, runtime.permissions_storage_.get(), input.approval_callback, input.edit_mode);
        runtime.mcp_manager = std::move(tool_bootstrap.mcp_manager);

        runtime.system_prompt = append_subagent_prompt_guidance(input.system_prompt, input.allowed_child_agents, input.is_child_run);

        SkillLoader skill_loader;
        skill_loader.load_from_directories(resolve_skill_directories(input.skill_paths, input.workspace_root));
        runtime.skills_prompt = skill_loader.build_prompt_section();

        runtime.hook_manager = std::make_unique<HookManager>();
        runtime.hook_manager->load_from_directories(resolve_hook_directories(input));

        runtime.agent =
            std::make_unique<AgentLoop>(*runtime.provider, runtime.tools, runtime.system_prompt, runtime.memory.get(), runtime.skills_prompt, runtime.hook_manager.get());
        runtime.agent->set_thinking_budget(input.thinking_budget);
        return runtime;
    }

} // namespace orangutan
