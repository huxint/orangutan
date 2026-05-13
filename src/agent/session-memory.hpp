#pragma once

#include "agent/agent-loop.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace orangutan::agent {

    [[nodiscard]]
    std::string render_prompt_memory_section(memory::RuntimeMemory *memory, std::string_view user_input);

    [[nodiscard]]
    AgentLoop::SessionMemoryDistillationResult distill_session_memory_from_history(ProviderSystem &provider, const ProviderRoute &route, memory::RuntimeMemory *memory,
                                                                                   const std::vector<Message> &history);

} // namespace orangutan::agent
