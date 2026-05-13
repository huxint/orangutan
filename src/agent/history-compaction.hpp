#pragma once

#include "agent/agent-loop.hpp"

#include <vector>

namespace orangutan::agent {

    [[nodiscard]]
    AgentLoop::HistoryCompactionResult compact_conversation_history(ProviderSystem &provider, const ProviderRoute &route, std::vector<Message> &history);

} // namespace orangutan::agent
