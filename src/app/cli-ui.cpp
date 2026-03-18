#include "app/cli-ui.hpp"

#include <algorithm>
#include <sstream>

namespace orangutan::app {

std::string repl_help_text() {
    return "Commands:\n"
           "  /help            - show this help\n"
           "  /new             - save current session and start a new one\n"
           "  /compress        - summarize older history and keep recent messages verbatim\n"
           "  /clear           - clear conversation history\n"
           "  /history         - show conversation summary\n"
           "  /session         - show the current session id\n"
           "  /sessions        - list saved sessions for the current agent scope\n"
           "  /load <id>       - load a saved session\n"
           "  /resume <id>     - alias of /load; supports 'latest'\n"
           "  /tools           - list all registered tools\n"
           "  /skills          - list loaded skills\n"
           "  /agent           - show the current agent\n"
           "  /agents          - list configured agents\n"
           "  /multi           - enter multi-line mode (end with empty line)\n"
           "  /save            - save current session\n"
           "  /quit            - exit\n\n";
}

std::string channel_help_text() {
    return "Commands:\n"
           "/help - show this help\n"
           "/new - start a new session\n"
           "/compress - summarize older history\n"
           "/session - show the current session id\n"
           "/sessions - list saved sessions in this scope\n"
           "/resume <id> - resume a saved session (or 'latest')\n"
           "/agent - show the current agent\n"
           "/agents - list configured agents";
}

std::string format_agent_list(const Config &cfg, const std::string &current_agent_key) {
    if (cfg.agents.empty()) {
        return "No configured agents.";
    }

    std::ostringstream out;
    out << "Configured agents:\n";
    for (const auto &[agent_key, agent_cfg] : cfg.agents) {
        out << "- " << agent_key;
        if (agent_key == current_agent_key) {
            out << " [current]";
        }
        out << " | model=" << agent_cfg.model;
        if (!agent_cfg.workspace.empty()) {
            out << " | workspace=" << agent_cfg.workspace;
        }
        if (!agent_cfg.subagents.empty()) {
            out << " | subagents=";
            for (size_t index = 0; index < agent_cfg.subagents.size(); ++index) {
                if (index > 0) {
                    out << ',';
                }
                out << agent_cfg.subagents[index];
            }
        }
        out << '\n';
    }
    return out.str();
}

std::string format_current_session(const std::string &current_session_id, const std::string &agent_key) {
    if (current_session_id.empty()) {
        return "No active session. Send a message to start one.";
    }
    return "Current session: " + current_session_id + " (agent: " + agent_key + ')';
}

std::string format_scoped_sessions(const std::vector<SessionInfo> &sessions, const std::string &current_session_id) {
    if (sessions.empty()) {
        return "No saved sessions for this conversation scope.";
    }

    std::ostringstream out;
    out << "Saved sessions for this conversation:\n";
    constexpr size_t max_sessions_to_show = 10;
    const auto count = std::min(max_sessions_to_show, sessions.size());
    for (size_t index = 0; index < count; ++index) {
        const auto &session = sessions[index];
        out << "- " << session.id;
        if (session.id == current_session_id) {
            out << " [current]";
        }
        out << " | " << session.created_at << " | " << session.model << " | " << session.message_count << " messages\n";
    }
    if (sessions.size() > count) {
        out << "... and " << (sessions.size() - count) << " more";
    }
    return out.str();
}

std::string render_history_summary(const AgentLoop &agent) {
    const auto &history = agent.history();
    if (history.empty()) {
        return "(no conversation history)\n\n";
    }

    std::ostringstream out;
    for (size_t index = 0; index < history.size(); ++index) {
        const auto &message = history[index];
        out << '[' << index << "] " << message.role << ": ";

        for (const auto &block : message.content) {
            if (const auto *text = std::get_if<TextBlock>(&block)) {
                auto preview = text->text.substr(0, 80);
                if (text->text.size() > 80) {
                    preview += "...";
                }
                out << preview;
            } else if (const auto *tool = std::get_if<ToolUseBlock>(&block)) {
                out << "[tool_use: " << tool->name << ']';
            } else if (std::get_if<ToolResultBlock>(&block) != nullptr) {
                out << "[tool_result]";
            }
        }
        out << '\n';
    }
    out << '\n';
    return out.str();
}

std::string render_saved_sessions(SessionStore &store, const std::string &scope_key) {
    const auto sessions = store.list_sessions(scope_key);
    if (sessions.empty()) {
        return "(no saved sessions)\n\n";
    }

    std::ostringstream out;
    out << "Saved sessions:\n";
    for (const auto &session : sessions) {
        out << "  " << session.id << "  " << session.created_at << "  " << session.model << "  (" << session.message_count << " messages)\n";
    }
    out << '\n';
    return out.str();
}

} // namespace orangutan::app
