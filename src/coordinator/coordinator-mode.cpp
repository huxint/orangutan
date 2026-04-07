#include "coordinator/coordinator-mode.hpp"

#include "tools/registry/tool-context.hpp"

namespace orangutan::coordinator {

    bool is_coordinator_mode(const tools::ToolRuntimeContext *tool_context) {
        return tool_context != nullptr && tool_context->coordinator_mode && !tool_context->is_child_run;
    }

} // namespace orangutan::coordinator
