#pragma once

#include "features/agent/agent-loop.hpp"
#include "infra/config/config.hpp"
#include "infra/storage/session-store.hpp"

#include <functional>
#include <iosfwd>
#include <string>

namespace orangutan::app {

using JsonEmitter = std::function<void(const json &event)>;

void emit_session_history_dump(const std::vector<Message> &history, const std::string &current_session_id, const JsonEmitter &emit);

int run_single_message(AgentLoop &agent, SessionStore &session_store, const Config &cfg, const std::string &message, bool event_stream, std::string &current_session_id,
                       const std::string &model, const std::string &scope_key, const JsonEmitter &emit, std::ostream &error_stream);

} // namespace orangutan::app
