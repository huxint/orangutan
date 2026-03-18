#pragma once

#include "features/agent/agent-loop.hpp"
#include "infra/config/config.hpp"
#include "features/skills/skill-loader.hpp"
#include "core/tools/tool.hpp"

#include <string>

namespace orangutan {

class HookManager;
class SessionStore;

}

namespace orangutan::app {

void run_repl(AgentLoop &agent, SessionStore &store, const std::string &model, const Config &cfg, std::string &current_session_id, const std::string &agent_key,
              const std::string &scope_key = {}, const SkillLoader *skill_loader = nullptr, const ToolRegistry *tool_registry = nullptr,
              HookManager *hook_manager = nullptr);

} // namespace orangutan::app
