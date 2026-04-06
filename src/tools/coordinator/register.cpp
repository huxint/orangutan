#include "tools/coordinator/register.hpp"
#include "tools/internal.hpp"

#include <array>

namespace orangutan::tools {

    namespace {
        constexpr std::array COORDINATOR_TOOL_REGISTRARS = {
            register_agent_spawn_tool,
            register_agent_send_message_tool,
            register_agent_stop_tool,
        };
    } // namespace

    void register_coordinator_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        register_contextual_tools(registry, tool_context, COORDINATOR_TOOL_REGISTRARS);
    }

} // namespace orangutan::tools
