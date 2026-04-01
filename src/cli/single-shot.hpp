#pragma once

#include "providers/provider.hpp"
#include "agent/agent-loop.hpp"
#include "config/config.hpp"
#include "storage/session-store.hpp"

#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>

namespace orangutan::automation {
    class Runtime;
}

namespace orangutan::cli {

    using JsonEmitter = std::function<void(const nlohmann::json &event)>;
    using CompletionResumePostRunCallback = std::function<std::optional<std::string>(const std::string &reply)>;

    void emit_session_history_dump(const std::vector<Message> &history, const std::string &current_session_id, const JsonEmitter &emit);
    std::optional<std::string> run_completion_resume_message(AgentLoop &agent, const std::string &message, std::string_view agent_key,
                                                             automation::Runtime *automation_runtime = nullptr, const CompletionResumePostRunCallback &post_run = {},
                                                             bool suppress_human_output = false);

    int run_single_message(AgentLoop &agent, const Provider &provider, SessionStore &session_store, const Config &cfg, const std::string &message, bool event_stream,
                           std::string &current_session_id, const std::string &configured_model, const std::string &scope_key, const std::string &agent_key,
                           const JsonEmitter &emit, std::ostream &error_stream, automation::Runtime *automation_runtime = nullptr);

} // namespace orangutan::cli
