#include "bootstrap/automation-executor.hpp"

#include "agent/agent-loop.hpp"
#include "automation/runtime.hpp"
#include "bootstrap/runtime-control.hpp"
#include "bootstrap/runtime-factory.hpp"
#include "config/config.hpp"
#include "orchestration/mailbox.hpp"
#include "orchestration/orchestration-manager.hpp"
#include "orchestration/team-manager.hpp"
#include "utils/scope-exit.hpp"

#include <memory>
#include <stdexcept>

namespace orangutan::bootstrap {

    automation::ExecutionResult execute_automation_with_runtime(const automation::Automation &automation, const AutomationExecutorDependencies &deps) {
        automation::ExecutionResult result;
        if (deps.config == nullptr || deps.agent_runtime_configs == nullptr || deps.automation_runtime == nullptr) {
            result.summary = "automation runtime executor is not fully configured";
            return result;
        }

        const auto config_it = deps.agent_runtime_configs->find(automation.agent_key);
        if (config_it == deps.agent_runtime_configs->end()) {
            result.summary = "No runtime configuration for agent '" + automation.agent_key + "'.";
            return result;
        }

        const auto &runtime_cfg = config_it->second;
        std::string current_session_id;
        auto completion_resume_state = std::make_shared<RuntimeCompletionResumeState>();
        completion_resume_state->agent_key = runtime_cfg.agent_key;
        completion_resume_state->configured_model = runtime_cfg.model;
        completion_resume_state->scope_key = "agent:" + runtime_cfg.agent_key + "|automation";
        completion_resume_state->automation_runtime = deps.automation_runtime;
        completion_resume_state->suppress_human_output = true;
        RuntimeIdentity identity{
            .workspace = runtime_cfg.workspace_root,
            .runtime_key = "agent:" + runtime_cfg.agent_key + "|automation:" + automation.id,
            .memory_scope = "agent:" + runtime_cfg.agent_key + "|automation",
        };

        try {
            auto runtime = build_runtime_bundle(RuntimeFactoryRequest{
                .runtime_config = &runtime_cfg,
                .identity = &identity,
                .app_config = deps.config,
                .memory_store = deps.memory_store,
                .current_session_id = &current_session_id,
                .orchestration_manager = deps.orchestration_manager,
                .team_manager = deps.team_manager,
                .mailbox = deps.mailbox,
                .runtime_origin = base::origin::cli,
                .raw_caller_id = identity.runtime_key,
                .automation_service = &deps.automation_runtime->service(),
                .automation_runtime = deps.automation_runtime,
                .background_completion_runtime = make_runtime_background_completion_bindings(deps.automation_runtime, make_runtime_completion_resume_callback(completion_resume_state)),
            });
            const auto completion_resume_guard = utils::scope_exit([completion_resume_state] {
                deactivate_runtime_completion_resume_state(completion_resume_state);
            });
            completion_resume_state->agent = runtime.agent.get();
            completion_resume_state->provider = runtime.provider.get();
            result.reply = runtime.agent->run(automation.prompt);
            result.summary = result.reply;
            result.workspace_root = runtime_cfg.workspace_root;
            result.success = true;
            return result;
        } catch (const std::exception &e) {
            result.summary = e.what();
            result.workspace_root = runtime_cfg.workspace_root;
            return result;
        }
    }

    automation::AutomationExecutor make_bootstrap_automation_executor(AutomationExecutorDependencies deps) {
        return [deps](const automation::Automation &automation) {
            return execute_automation_with_runtime(automation, deps);
        };
    }

} // namespace orangutan::bootstrap
