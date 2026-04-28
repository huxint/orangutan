#pragma once

#include "automation/delivery.hpp"
#include "bootstrap/runtime-config.hpp"

#include <string>
#include <unordered_map>

namespace orangutan::config {
    struct Config;
}

namespace orangutan::automation {
    class AutomationRuntime;
} // namespace orangutan::automation

namespace orangutan::memory {
    class MemoryStore;
} // namespace orangutan::memory

namespace orangutan::orchestration {
    class AgentMailbox;
    class OrchestrationManager;
    class TeamManager;
} // namespace orangutan::orchestration

namespace orangutan::bootstrap {

    struct AutomationExecutorDependencies {
        const config::Config *config = nullptr;
        const std::unordered_map<std::string, AgentRuntimeConfig> *agent_runtime_configs = nullptr;
        memory::MemoryStore *memory_store = nullptr;
        orchestration::OrchestrationManager *orchestration_manager = nullptr;
        orchestration::TeamManager *team_manager = nullptr;
        orchestration::AgentMailbox *mailbox = nullptr;
        automation::AutomationRuntime *automation_runtime = nullptr;
    };

    [[nodiscard]]
    automation::ExecutionResult execute_automation_with_runtime(const automation::Automation &automation, const AutomationExecutorDependencies &deps);

    [[nodiscard]]
    automation::AutomationExecutor make_bootstrap_automation_executor(AutomationExecutorDependencies deps);

} // namespace orangutan::bootstrap
