#pragma once

#include "agent/agent-loop.hpp"
#include "providers/provider.hpp"
#include "infra/config/config.hpp"
#include "skills/skill-loader.hpp"
#include "tools/registry/tool.hpp"

#include <string>
#include <vector>

namespace orangutan {

    class HookManager;
    class SessionStore;

    namespace automation {
        class Runtime;
    }

} // namespace orangutan

namespace orangutan::app {

    void run_repl(AgentLoop &agent, const Provider &provider, SessionStore &store, const std::string &configured_model, const std::vector<std::string> &fallback_models,
                  const Config &cfg, std::string &current_session_id, const std::string &agent_key, const std::string &scope_key = {}, const std::string &workspace_root = {},
                  const SkillLoader *skill_loader = nullptr, const ToolRegistry *tool_registry = nullptr, HookManager *hook_manager = nullptr,
                  automation::Runtime *automation_runtime = nullptr);

} // namespace orangutan::app
