#include "coordinator/coordinator-mode.hpp"

#include "tools/registry/tool-context.hpp"

#include <array>
#include <string>

namespace orangutan::coordinator {

    bool is_coordinator_mode(const tools::ToolRuntimeContext *tool_context) {
        return tool_context != nullptr && tool_context->coordinator_mode && !tool_context->is_child_run;
    }

    const std::vector<std::string> &get_coordinator_allowed_tools() {
        static const std::vector<std::string> tools = {
            "agent_spawn",
            "agent_send_message",
            "agent_stop",
        };
        return tools;
    }

} // namespace orangutan::coordinator
