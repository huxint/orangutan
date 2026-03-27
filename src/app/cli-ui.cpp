#include "app/cli-ui.hpp"
#include "app/slash-commands.hpp"

#include <spdlog/common.h>
#include <algorithm>
#include "infra/format.hpp"

namespace orangutan::app {

std::string repl_help_text() {
    return render_slash_help_text(slash_command_surface::cli);
}

std::string channel_help_text() {
    return render_slash_help_text(slash_command_surface::channel);
}

std::string web_help_text() {
    return render_slash_help_text(slash_command_surface::web);
}

std::string format_agent_list(const Config &cfg, const std::string &current_agent_key) {
    if (cfg.agents.empty()) {
        return "## Agents\n- 💤 No configured agents.";
    }

    std::string out = "## Agents\n";
    for (const auto &[agent_key, agent_cfg] : cfg.agents) {
        append(out, "- 🤖 `{}`", agent_key);
        if (agent_key == current_agent_key) {
            out += " **(current)**";
        }
        append(out, " — model: `{}`", agent_cfg.model);
        if (!agent_cfg.workspace.empty()) {
            append(out, ", workspace: `{}`", agent_cfg.workspace);
        }
        if (!agent_cfg.subagents.empty()) {
            out += ", subagents: ";
            for (size_t index = 0; index < agent_cfg.subagents.size(); ++index) {
                if (index > 0) {
                    out.push_back(',');
                }
                append(out, "`{}`", agent_cfg.subagents[index]);
            }
        }
        out.push_back('\n');
    }
    return out;
}

std::string format_current_agent(const std::string &agent_key) {
    return "## Agent\n- 🤖 Current Agent: `" + agent_key + "`";
}

std::string format_current_session(const std::string &current_session_id, const std::string &agent_key) {
    if (current_session_id.empty()) {
        return "## Session\n- 💤 No active session yet.\n- 🤖 Agent: `" + agent_key + "`";
    }
    return "## Session\n- 🧵 Current Session: `" + current_session_id + "`\n- 🤖 Agent: `" + agent_key + "`";
}

std::string format_scoped_sessions(const std::vector<SessionInfo> &sessions, const std::string &current_session_id) {
    if (sessions.empty()) {
        return "## Sessions\n- 💤 No saved sessions for this conversation scope.";
    }

    std::string out = "## Sessions\n";
    constexpr size_t max_sessions_to_show = 10;
    const auto count = std::min(max_sessions_to_show, sessions.size());
    for (size_t index = 0; index < count; ++index) {
        const auto &session = sessions[index];
        append(out, "- 🧵 `{}`", session.id);
        if (session.id == current_session_id) {
            out += " **(current)**";
        }
        append(out, " — {}, model: `{}`, messages: `{}`\n", session.created_at, session.model, session.message_count);
    }
    if (sessions.size() > count) {
        append(out, "- ➕ And `{}` more", sessions.size() - count);
    }
    return out;
}

std::string format_history_compaction_result(const AgentLoop::HistoryCompactionResult &result) {
    if (!result.compacted) {
        return "## Compression\n- " + result.status;
    }

    return spdlog::fmt_lib::format("## Compression\n- Messages: `{} -> {}`", result.messages_before, result.messages_after);
}

std::string render_saved_sessions(SessionStore &store, const std::string &scope_key) {
    const auto sessions = store.list_sessions(scope_key);
    if (sessions.empty()) {
        return "(no saved sessions)\n\n";
    }

    std::string out = "🗂️ Saved sessions:\n";
    for (const auto &session : sessions) {
        append(out, "  {}  {}  {}  ({} messages)\n", session.id, session.created_at, session.model, session.message_count);
    }
    out.push_back('\n');
    return out;
}

RuntimeStatusSnapshot collect_runtime_status(const AgentLoop &agent, const Provider &provider, const ToolRegistry *tool_registry, const std::string &current_session_id,
                                             const std::string &agent_key, const std::string &configured_model, const std::vector<std::string> &fallback_models,
                                             const std::string &scope_key) {
    RuntimeStatusSnapshot status{
        .agent_key = agent_key,
        .provider_name = provider.name(),
        .current_model = provider.current_model().empty() ? configured_model : provider.current_model(),
        .configured_model = configured_model,
        .fallback_models = fallback_models,
        .current_session_id = current_session_id,
        .scope_key = scope_key,
        .history_messages = agent.history().size(),
        .provider_usage = provider.usage(),
    };

    for (const auto &message : agent.history()) {
        if (message.role == Role::user) {
            ++status.user_messages;
        } else if (message.role == Role::assistant) {
            ++status.assistant_messages;
        }

        for (const auto &block : message.content) {
            if (std::get_if<ToolUseBlock>(&block) != nullptr) {
                ++status.tool_call_count;
            } else if (const auto *result = std::get_if<ToolResultBlock>(&block); result != nullptr && result->is_error) {
                ++status.tool_error_count;
            }
        }
    }

    if (tool_registry != nullptr) {
        status.registered_tool_count = tool_registry->definitions().size();
    }

    return status;
}

std::string format_runtime_status(const RuntimeStatusSnapshot &status) {
    std::string out = "## Status\n";
    append(out, "- 🤖 Agent: `{}`\n", status.agent_key);
    append(out, "- 🔌 Provider: `{}`\n", status.provider_name);
    append(out, "- 🧠 Model: `{}`\n", status.current_model);
    if (!status.configured_model.empty() && status.configured_model != status.current_model) {
        append(out, "- 🎯 Configured Model: `{}`\n", status.configured_model);
    }
    if (!status.fallback_models.empty()) {
        out += "- 🔁 Fallback Models: ";
        for (size_t index = 0; index < status.fallback_models.size(); ++index) {
            if (index > 0) {
                out += ", ";
            }
            append(out, "`{}`", status.fallback_models[index]);
        }
        out.push_back('\n');
    }
    append(out, "- 🧵 Session: `{}`\n", status.current_session_id.empty() ? "none" : status.current_session_id);
    if (!status.scope_key.empty()) {
        append(out, "- 🗂️ Scope: `{}`\n", status.scope_key);
    }
    append(out, "- 📊 Usage: `llm_requests={}`, `attempts={}`, `fallbacks={}`, `failed_attempts={}`, `tool_calls={}`, `tool_errors={}`\n", status.provider_usage.logical_requests,
           status.provider_usage.attempt_count, status.provider_usage.fallback_switches, status.provider_usage.failed_attempts, status.tool_call_count, status.tool_error_count);
    append(out, "- 💬 History: `messages={}`, `user={}`, `assistant={}`\n", status.history_messages, status.user_messages, status.assistant_messages);
    append(out, "- 🛠️ Tools: `registered={}`", status.registered_tool_count);
    return out;
}

} // namespace orangutan::app
