#include "cli/cli-ui.hpp"
#include "cli/slash-commands.hpp"

#include <spdlog/common.h>
#include <algorithm>
#include <magic_enum/magic_enum.hpp>
#include "utils/format.hpp"

namespace orangutan::cli {

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
            utils::format_to(out, "- 🤖 `{}`", agent_key);
            if (agent_key == current_agent_key) {
                out += " **(current)**";
            }
            utils::format_to(out, " — model: `{}`", agent_cfg.model);
            if (!agent_cfg.workspace.empty()) {
                utils::format_to(out, ", workspace: `{}`", agent_cfg.workspace);
            }
            if (!agent_cfg.team_agents.empty()) {
                out += ", team_agents: ";
                for (std::size_t index = 0; index < agent_cfg.team_agents.size(); ++index) {
                    if (index > 0) {
                        out.push_back(',');
                    }
                    utils::format_to(out, "`{}`", agent_cfg.team_agents[index]);
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
        constexpr std::size_t MAX_SESSIONS_TO_SHOW = 10;
        const auto count = std::min(MAX_SESSIONS_TO_SHOW, sessions.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto &session = sessions[index];
            utils::format_to(out, "- 🧵 `{}`", session.id);
            if (session.id == current_session_id) {
                out += " **(current)**";
            }
            utils::format_to(out, " — {}, model: `{}`, messages: `{}`\n", session.created_at, session.model, session.message_count);
        }
        if (sessions.size() > count) {
            utils::format_to(out, "- ➕ And `{}` more", sessions.size() - count);
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
            utils::format_to(out, "  {}  {}  {}  ({} messages)\n", session.id, session.created_at, session.model, session.message_count);
        }
        out.push_back('\n');
        return out;
    }

    std::string format_skill_catalog(const skills::skill_catalog_view &catalog) {
        if (catalog.skills.empty()) {
            return "## Skills\n- 💤 No skills loaded.";
        }

        std::vector<const skills::skill_view *> ordered;
        ordered.reserve(catalog.skills.size());
        for (const auto &skill : catalog.skills) {
            ordered.push_back(&skill);
        }
        std::ranges::sort(ordered, [](const skills::skill_view *left, const skills::skill_view *right) {
            return left->name < right->name;
        });

        std::string out = "## Skills\n";
        for (const auto *skill : ordered) {
            utils::format_to(out, "- 🧠 `{}` — {}\n", skill->name, skill->description);
            const auto source_name = magic_enum::enum_name(skill->source);
            const auto scope_name = magic_enum::enum_name(skill->scope);
            utils::format_to(out, "  source: `{}`, scope: `{}`, active: `{}`\n", source_name, scope_name, skill->active ? "yes" : "no");
            if (!skill->tools.empty()) {
                out += "  tools: ";
                for (std::size_t index = 0; index < skill->tools.size(); ++index) {
                    if (index > 0) {
                        out += ", ";
                    }
                    utils::format_to(out, "`{}`", skill->tools[index]);
                }
                out.push_back('\n');
            }
            if (!skill->source_path.empty()) {
                utils::format_to(out, "  path: `{}`\n", skill->source_path);
            }
        }
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
            if (message.role() == base::role::user) {
                ++status.user_messages;
            } else if (message.role() == base::role::assistant) {
                ++status.assistant_messages;
            }

            for (const auto &block : message) {
                if (std::get_if<ToolUse>(&block) != nullptr) {
                    ++status.tool_call_count;
                } else if (const auto *result = std::get_if<ToolResult>(&block); result != nullptr && result->is_error) {
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
        utils::format_to(out, "- 🤖 Agent: `{}`\n", status.agent_key);
        utils::format_to(out, "- 🔌 Provider: `{}`\n", status.provider_name);
        utils::format_to(out, "- 🧠 Model: `{}`\n", status.current_model);
        if (!status.configured_model.empty() && status.configured_model != status.current_model) {
            utils::format_to(out, "- 🎯 Configured Model: `{}`\n", status.configured_model);
        }
        if (!status.fallback_models.empty()) {
            out += "- 🔁 Fallback Models: ";
            for (std::size_t index = 0; index < status.fallback_models.size(); ++index) {
                if (index > 0) {
                    out += ", ";
                }
                utils::format_to(out, "`{}`", status.fallback_models[index]);
            }
            out.push_back('\n');
        }
        utils::format_to(out, "- 🧵 Session: `{}`\n", status.current_session_id.empty() ? "none" : status.current_session_id);
        if (!status.scope_key.empty()) {
            utils::format_to(out, "- 🗂️ Scope: `{}`\n", status.scope_key);
        }
        utils::format_to(out, "- 📊 Usage: `llm_requests={}`, `attempts={}`, `fallbacks={}`, `failed_attempts={}`, `tool_calls={}`, `tool_errors={}`\n",
                         status.provider_usage.logical_requests, status.provider_usage.attempt_count, status.provider_usage.fallback_switches,
                         status.provider_usage.failed_attempts, status.tool_call_count, status.tool_error_count);
        utils::format_to(out, "- 💬 History: `messages={}`, `user={}`, `assistant={}`\n", status.history_messages, status.user_messages, status.assistant_messages);
        utils::format_to(out, "- 🛠️ Tools: `registered={}`", status.registered_tool_count);
        return out;
    }

} // namespace orangutan::cli
