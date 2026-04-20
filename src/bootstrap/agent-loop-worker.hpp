#pragma once

#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/channel-serve-runtime.hpp"
#include "config/config.hpp"
#include "orchestration/orchestration-manager.hpp"
#include "orchestration/worker-runtime.hpp"

#include <string>
#include <unordered_map>

namespace orangutan::memory {
    class MemoryStore;
}

namespace orangutan::bootstrap {

    /// Build a `WorkerRuntimeFactory` that produces `AgentLoop`-backed workers for
    /// the orchestration manager. The returned factory holds references to the
    /// supplied arguments; callers must keep them alive for the manager's lifetime.
    [[nodiscard]]
    orchestration::WorkerRuntimeFactory make_agent_loop_worker_factory(const Config &cfg,
                                                                        const std::unordered_map<std::string, AgentRuntimeConfig> &agent_runtime_configs,
                                                                        memory::MemoryStore *memory_store,
                                                                        orchestration::OrchestrationManager &orchestration_manager);

} // namespace orangutan::bootstrap
