#include "orchestration/leader-mode.hpp"

#include "tools/registry/tool-context.hpp"

namespace orangutan::orchestration {

    bool is_leader_mode(const tools::ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return false;
        }
        // Prefer the new agent_role enum; fall back to legacy bools for compatibility.
        if (tool_context->role == orchestration::agent_role::leader) {
            return true;
        }
        return tool_context->coordinator_mode && !tool_context->is_child_run;
    }

} // namespace orangutan::orchestration
