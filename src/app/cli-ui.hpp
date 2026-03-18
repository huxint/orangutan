#pragma once

#include "features/agent/agent-loop.hpp"
#include "infra/config/config.hpp"
#include "infra/storage/session-store.hpp"

#include <string>

namespace orangutan::app {

[[nodiscard]]
std::string repl_help_text();
[[nodiscard]]
std::string channel_help_text();
[[nodiscard]]
std::string format_agent_list(const Config &cfg, const std::string &current_agent_key);
[[nodiscard]]
std::string format_current_session(const std::string &current_session_id, const std::string &agent_key);
[[nodiscard]]
std::string format_scoped_sessions(const std::vector<SessionInfo> &sessions, const std::string &current_session_id);
[[nodiscard]]
std::string render_history_summary(const AgentLoop &agent);
[[nodiscard]]
std::string render_saved_sessions(SessionStore &store, const std::string &scope_key = {});

} // namespace orangutan::app
