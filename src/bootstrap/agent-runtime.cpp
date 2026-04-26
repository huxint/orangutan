#include "bootstrap/agent-runtime.hpp"

#include "bootstrap/config-bootstrap.hpp"
#include "bootstrap/memory-context.hpp"
#include "bootstrap/identity.hpp"
#include "orchestration/leader-prompt.hpp"
#include "permissions/permission-state.hpp"
#include "prompt/system-prompt-sections.hpp"
#include "providers/provider.hpp"
#include "agent/agent-loop.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/runtime-memory.hpp"
#include "skills/skill-loader.hpp"
#include "tools/file/edit/edit-mode.hpp"
#include "tools/runtime-loader/runtime-loader.hpp"
#include "tools/skill/skill-tool.hpp"
#include "utils/enum-string.hpp"
#include "utils/format.hpp"

#include <utility>
#include <vector>

namespace orangutan::bootstrap {
    namespace {

        void append_prompt_section(std::string &prompt, std::string section) {
            if (section.empty()) {
                return;
            }
            if (!prompt.empty()) {
                prompt += "\n\n";
            }
            prompt += std::move(section);
        }

        [[nodiscard]]
        std::string append_mcp_tool_search_guidance(std::string message) {
            message += " Use `tool_search` when you need specific MCP tools.";
            return message;
        }

        [[nodiscard]]
        std::string render_orchestration_capability_section(const ToolRuntimeContext &tool_context) {
            if (tool_context.orchestration_manager == nullptr || orchestration::is_teammate(tool_context.role)) {
                return {};
            }

            std::string out = "## Agent Orchestration\n";
            out += "Agent orchestration is loaded in this process. You can create persistent teammates with `agent_spawn`, "
                   "send follow-up messages with `agent_send_message`, stop agents with `agent_stop`, and organize teammates with `team_create`/`team_delete` when useful.\n";
            out += "There is no preset teammate catalog or allowlist: choose each spawned agent's `name`, `task`, and `instructions` for the current work.\n";
            out += "Use `relationship: \"managed\"` for assigned execution and `relationship: \"peer\"` for discussion or coordination.\n";
            out += "Spawned agents inherit this runtime by default. You may override `profile`, `model`, or `thinking_budget` per spawn when the configured profile/model exists.";
            return out;
        }

        [[nodiscard]]
        std::string render_mcp_capability_section(const AgentRuntimeBuildInput &input, const tools::McpManager *mcp_manager) {
            std::string out = "## MCP Tools\n";
            if (input.mcp_servers.empty()) {
                out += append_mcp_tool_search_guidance("No MCP servers are configured for this runtime.");
                return out;
            }

            if (mcp_manager == nullptr) {
                out += append_mcp_tool_search_guidance("MCP servers are configured, but no MCP manager is loaded for this runtime role.");
                return out;
            }

            utils::format_to(out, "{} MCP servers configured; {} connected; {} tools registered.", input.mcp_servers.size(), mcp_manager->connected_server_count(),
                             mcp_manager->total_tool_count());
            out = append_mcp_tool_search_guidance(std::move(out));
            return out;
        }

        [[nodiscard]]
        std::string render_memory_capability_section(RuntimeMemory *memory) {
            std::string out = "## Long-Term Memory\n";
            if (memory == nullptr) {
                out += "Long-term memory is disabled for this runtime.";
                return out;
            }

            utils::format_to(out, "Long-term memory is loaded for scope `{}`. Relevant memories are injected automatically when available.", memory->context().scope);
            return out;
        }

        [[nodiscard]]
        std::string render_runtime_capability_section(const AgentRuntimeBuildInput &input, const AgentRuntimeBundle &runtime) {
            std::string out;
            append_prompt_section(out, render_orchestration_capability_section(runtime.tool_context()));
            append_prompt_section(out, render_mcp_capability_section(input, runtime.mcp_manager.get()));
            append_prompt_section(out, render_memory_capability_section(runtime.memory.get()));
            return out;
        }

    } // namespace

    AgentRuntimeBundle::AgentRuntimeBundle()
    : tool_context_storage_(std::make_unique<ToolRuntimeContext>()),
      tools_storage_(std::make_unique<ToolRegistry>()),
      permissions_storage_(std::make_unique<ToolPermissionContext>()) {}

    AgentRuntimeBundle::~AgentRuntimeBundle() = default;

    AgentRuntimeBundle::AgentRuntimeBundle(AgentRuntimeBundle &&other) noexcept
    : tool_context_storage_(other.tool_context_storage_ != nullptr ? std::move(other.tool_context_storage_) : std::make_unique<ToolRuntimeContext>()),
      tools_storage_(other.tools_storage_ != nullptr ? std::move(other.tools_storage_) : std::make_unique<ToolRegistry>()),
      permissions_storage_(other.permissions_storage_ != nullptr ? std::move(other.permissions_storage_) : std::make_unique<ToolPermissionContext>()),
      active_hook_manager_(std::exchange(other.active_hook_manager_, nullptr)),
      provider(std::move(other.provider)),
      memory(std::move(other.memory)),
      mcp_manager(std::move(other.mcp_manager)),
      skills_prompt(std::move(other.skills_prompt)),
      skill_loader(std::move(other.skill_loader)),
      hook_manager(std::move(other.hook_manager)),
      agent(std::move(other.agent)) {}

    AgentRuntimeBundle &AgentRuntimeBundle::operator=(AgentRuntimeBundle &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        tool_context_storage_ = other.tool_context_storage_ != nullptr ? std::move(other.tool_context_storage_) : std::make_unique<ToolRuntimeContext>();
        tools_storage_ = other.tools_storage_ != nullptr ? std::move(other.tools_storage_) : std::make_unique<ToolRegistry>();
        permissions_storage_ = other.permissions_storage_ != nullptr ? std::move(other.permissions_storage_) : std::make_unique<ToolPermissionContext>();
        active_hook_manager_ = std::exchange(other.active_hook_manager_, nullptr);
        provider = std::move(other.provider);
        memory = std::move(other.memory);
        mcp_manager = std::move(other.mcp_manager);
        skills_prompt = std::move(other.skills_prompt);
        skill_loader = std::move(other.skill_loader);
        hook_manager = std::move(other.hook_manager);
        agent = std::move(other.agent);
        return *this;
    }

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

    hooks::HookManager *AgentRuntimeBundle::active_hook_manager() const noexcept {
        return active_hook_manager_;
    }

    void AgentRuntimeBundle::replace_permissions(ToolPermissionContext context) {
        *permissions_storage_ = std::move(context);
    }

    AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input) {
        AgentRuntimeBundle runtime;
        const bool leader_mode = orchestration::is_leader(input.agent_role);
        const bool teammate_mode = orchestration::is_teammate(input.agent_role);

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
            .orchestration_manager = input.orchestration_manager,
            .team_manager = input.team_manager,
            .mailbox = input.mailbox,
            .role = input.agent_role,
            .runtime_origin = input.runtime_origin,
            .raw_caller_id = input.raw_caller_id,
            .automation_service = input.automation_service,
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
        const auto edit_mode = utils::parse_enum_or(input.edit_mode, tools::file::DEFAULT_EDIT_MODE);
        auto tool_bootstrap = register_runtime_tools(runtime.tools(), runtime.memory.get(), std::filesystem::path(input.identity.workspace), runtime.tool_context_storage_.get(),
                                                     input.custom_tools, input.mcp_servers, runtime.permissions_storage_.get(), edit_mode);
        runtime.mcp_manager = std::move(tool_bootstrap.mcp_manager);

        runtime.skill_loader = std::make_unique<SkillLoader>();
        runtime.skill_loader->set_workspace_root(std::filesystem::path{input.workspace_root});
        runtime.skill_loader->load_from_directories(resolve_skill_directories(input.skill_paths, std::filesystem::path{input.workspace_root}));
        std::string prompt_guidance;
        if (leader_mode) {
            prompt_guidance = orchestration::get_leader_system_prompt();
        } else if (teammate_mode) {
            const auto task_description = input.delegated_task_prompt.empty() ? std::string("Complete the delegated task and report the result.") : input.delegated_task_prompt;
            const auto agent_name = input.agent_name.empty() ? input.agent_key : input.agent_name;
            prompt_guidance = orchestration::get_teammate_system_prompt_addendum(agent_name, task_description);
        } else if (input.orchestration_manager != nullptr) {
            prompt_guidance = append_agent_prompt_guidance({}, input.agent_role);
        }

        runtime.skills_prompt = std::move(prompt_guidance);
        append_prompt_section(runtime.skills_prompt, render_runtime_capability_section(input, runtime));
        auto skills_prompt = skills::render_skill_prompt_section_or_fallback(runtime.skill_loader->list(skills::skill_list_query{.include_inactive = false}));
        append_prompt_section(runtime.skills_prompt, std::move(skills_prompt));
        if (!leader_mode) {
            tools::register_skill_tool(runtime.tools(), *runtime.skill_loader);
        }

        if (input.hook_manager != nullptr) {
            runtime.active_hook_manager_ = input.hook_manager;
        } else {
            runtime.hook_manager = std::make_unique<HookManager>();
            runtime.hook_manager->load_from_directories(resolve_runtime_hook_dirs(input.hook_paths, input.workspace_root));
            runtime.active_hook_manager_ = runtime.hook_manager.get();
        }

        runtime.agent = std::make_unique<AgentLoop>(*runtime.provider, input.provider_route, runtime.tools(), runtime.memory.get(), runtime.skills_prompt,
                                                    runtime.active_hook_manager_, runtime.skill_loader.get());
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
