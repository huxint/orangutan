#pragma once

#include <stop_token>
#include <string>
#include <vector>

#include "orchestration/types.hpp"

namespace orangutan::orchestration {

    /// Abstract interface for executing a spawned teammate.
    ///
    /// A teammate executes the initial task, then may remain alive for
    /// follow-up prompts delivered through the team mailbox.
    class TeammateRuntime {
    public:
        virtual ~TeammateRuntime() = default;
        TeammateRuntime() = default;
        TeammateRuntime(const TeammateRuntime &) = delete;
        TeammateRuntime &operator=(const TeammateRuntime &) = delete;
        TeammateRuntime(TeammateRuntime &&) = delete;
        TeammateRuntime &operator=(TeammateRuntime &&) = delete;

        /// Execute the teammate with the given prompt. Returns the final text output.
        /// This may be called multiple times, once per prompt cycle.
        virtual auto run(const std::string &prompt, std::stop_token stop_token) -> std::string = 0;

        /// Wait for the next prompt from the leader or another teammate.
        /// Returns std::nullopt if the agent should shut down (stop requested
        /// or team dissolved). Returns the next prompt text otherwise.
        virtual auto wait_for_next_prompt(std::stop_token stop_token) -> std::optional<std::string> {
            static_cast<void>(stop_token);
            return std::nullopt;
        }

        /// Query whether this runtime can receive follow-up prompts.
        [[nodiscard]]
        virtual auto can_receive_followups() const -> bool { return false; }
    };

    /// Factory function type for creating TeammateRuntime instances.
    using TeammateRuntimeFactory = std::function<std::unique_ptr<TeammateRuntime>(const AgentSpawnRequest &request)>;

} // namespace orangutan::orchestration

namespace orangutan {
    using orchestration::TeammateRuntime;
    using orchestration::TeammateRuntimeFactory;
} // namespace orangutan
