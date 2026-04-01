#pragma once

#include "features/agent/agent-loop.hpp"
#include "providers/provider.hpp"
#include "tools/registry/tool.hpp"
#include "infra/config/config.hpp"
#include "infra/storage/session-store.hpp"

#include <string>
#include <vector>

namespace orangutan::app {

    struct RuntimeStatusSnapshot {
        std::string agent_key;
        std::string provider_name;
        std::string current_model;
        std::string configured_model;
        std::vector<std::string> fallback_models;
        std::string current_session_id;
        std::string scope_key;
        std::size_t history_messages = 0;
        std::size_t user_messages = 0;
        std::size_t assistant_messages = 0;
        std::size_t tool_call_count = 0;
        std::size_t tool_error_count = 0;
        std::size_t registered_tool_count = 0;
        ProviderUsageStats provider_usage;
    };

    [[nodiscard]]
    std::string repl_help_text();
    [[nodiscard]]
    std::string channel_help_text();
    [[nodiscard]]
    std::string web_help_text();
    [[nodiscard]]
    std::string format_agent_list(const Config &cfg, const std::string &current_agent_key);
    [[nodiscard]]
    std::string format_current_agent(const std::string &agent_key);
    [[nodiscard]]
    std::string format_current_session(const std::string &current_session_id, const std::string &agent_key);
    [[nodiscard]]
    std::string format_scoped_sessions(const std::vector<SessionInfo> &sessions, const std::string &current_session_id);
    [[nodiscard]]
    std::string format_history_compaction_result(const AgentLoop::HistoryCompactionResult &result);
    [[nodiscard]]
    std::string render_saved_sessions(SessionStore &store, const std::string &scope_key = {});
    [[nodiscard]]
    RuntimeStatusSnapshot collect_runtime_status(const AgentLoop &agent, const Provider &provider, const ToolRegistry *tool_registry, const std::string &current_session_id,
                                                 const std::string &agent_key, const std::string &configured_model, const std::vector<std::string> &fallback_models = {},
                                                 const std::string &scope_key = {});
    [[nodiscard]]
    std::string format_runtime_status(const RuntimeStatusSnapshot &status);

} // namespace orangutan::app
