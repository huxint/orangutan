#pragma once

#include <stop_token>
#include <string>
#include <vector>

#include "orchestration/types.hpp"

namespace orangutan::orchestration {

    /// Abstract interface for executing a spawned agent.
    ///
    /// Two lifecycle modes are supported via `AgentSpawnRequest::role`:
    /// - **worker** (fire-and-forget): `run()` executes one task and returns.
    /// - **teammate** (persistent): `run()` executes the initial task, then
    ///   the OrchestrationManager calls `wait_for_next_prompt()` to keep the
    ///   agent alive for follow-up work.
    class WorkerRuntime {
    public:
        virtual ~WorkerRuntime() = default;
        WorkerRuntime() = default;
        WorkerRuntime(const WorkerRuntime &) = delete;
        WorkerRuntime &operator=(const WorkerRuntime &) = delete;
        WorkerRuntime(WorkerRuntime &&) = delete;
        WorkerRuntime &operator=(WorkerRuntime &&) = delete;

        /// Execute the agent with the given prompt. Returns the final text output.
        /// For workers, this is called once. For teammates, this may be called
        /// multiple times (once per prompt cycle).
        virtual auto run(const std::string &prompt, std::stop_token stop_token) -> std::string = 0;

        /// For teammate-mode workers: wait for the next prompt from the leader.
        /// Returns std::nullopt if the agent should shut down (stop requested
        /// or team dissolved). Returns the next prompt text otherwise.
        ///
        /// Default implementation returns nullopt immediately (worker mode).
        virtual auto wait_for_next_prompt(std::stop_token stop_token) -> std::optional<std::string> {
            static_cast<void>(stop_token);
            return std::nullopt;
        }

        /// Query whether this runtime supports persistent (teammate) mode.
        [[nodiscard]]
        virtual auto is_persistent() const -> bool { return false; }
    };

    /// Factory function type for creating WorkerRuntime instances.
    using WorkerRuntimeFactory = std::function<std::unique_ptr<WorkerRuntime>(const AgentSpawnRequest &request)>;

} // namespace orangutan::orchestration

namespace orangutan {
    using orchestration::WorkerRuntime;
    using orchestration::WorkerRuntimeFactory;
} // namespace orangutan
