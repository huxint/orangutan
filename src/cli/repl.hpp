#pragma once

#include "agent/agent-loop.hpp"
#include "cli/session-workflow.hpp"
#include "providers/provider.hpp"
#include "config/config.hpp"
#include "skills/skill-loader.hpp"
#include "tools/registry/tool.hpp"

#include <string>
#include <vector>

namespace orangutan::automation {
    class AutomationRuntime;
}

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::storage {
    class SessionStore;
}

namespace orangutan::cli {

    void run_repl(AgentLoop &agent, const ProviderSystem &provider, storage::SessionStore &store, const std::string &configured_model,
                  const std::vector<std::string> &fallback_models,
                  const Config &cfg, std::string &current_session_id, const std::string &agent_key, const std::string &scope_key = {}, const std::string &workspace_root = {},
                  const SkillLoader *skill_loader = nullptr, const ToolRegistry *tool_registry = nullptr, hooks::HookManager *hook_manager = nullptr,
                  const SessionDistillationDispatcher &session_distillation_dispatcher = {}, automation::AutomationRuntime *automation_runtime = nullptr);

} // namespace orangutan::cli
