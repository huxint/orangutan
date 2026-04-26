#pragma once

#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/channel-serve-runtime.hpp"
#include "config/config.hpp"
#include "orchestration/orchestration-manager.hpp"
#include "orchestration/teammate-runtime.hpp"

#include <span>
#include <string>
#include <unordered_map>

namespace orangutan::memory {
    class MemoryStore;
}

namespace orangutan::orchestration {
    class AgentMailbox;
    struct MailboxMessage;
    class TeamManager;
}

namespace orangutan::bootstrap {

    namespace detail {
        [[nodiscard]]
        std::string format_teammate_messages_prompt(std::span<const orchestration::MailboxMessage> messages);
    } // namespace detail

    /// Build a `TeammateRuntimeFactory` that produces `AgentLoop`-backed teammates for
    /// the orchestration manager. The returned factory holds references to the
    /// supplied arguments; callers must keep them alive for the manager's lifetime.
    [[nodiscard]]
    orchestration::TeammateRuntimeFactory make_agent_loop_teammate_factory(const Config &cfg,
                                                                           const std::unordered_map<std::string, AgentRuntimeConfig> &agent_runtime_configs,
                                                                           memory::MemoryStore *memory_store,
                                                                           orchestration::OrchestrationManager &orchestration_manager,
                                                                           orchestration::TeamManager *team_manager,
                                                                           orchestration::AgentMailbox *mailbox);

} // namespace orangutan::bootstrap
