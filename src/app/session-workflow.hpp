#pragma once

#include "features/agent/agent-loop.hpp"
#include "infra/storage/session-store.hpp"

#include <optional>
#include <string>

namespace orangutan::app {

struct NewSessionResult {
    bool had_history = false;
    std::string previous_session_id;
    AgentLoop::SessionMemoryDistillationResult distillation;
};

struct LoadSessionResult {
    bool loaded = false;
    std::string status;
};

[[nodiscard]]
std::optional<std::string> resolve_requested_session(SessionStore &store, const std::string &requested, const std::string &scope_key);

[[nodiscard]]
bool persist_session(AgentLoop &agent, SessionStore &store, const std::string &model, std::string &current_session_id, const std::string &scope_key = {});

[[nodiscard]]
NewSessionResult start_new_session(AgentLoop &agent, SessionStore &store, const std::string &model, std::string &current_session_id, const std::string &scope_key = {});

[[nodiscard]]
LoadSessionResult load_session_into_agent(const std::string &requested_session_id, AgentLoop &agent, SessionStore &store, std::string &current_session_id,
                                          const std::string &scope_key = {});

[[nodiscard]]
std::string describe_new_session_result(const NewSessionResult &result, bool mention_previous_session);

} // namespace orangutan::app
