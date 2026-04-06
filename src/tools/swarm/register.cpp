#include "tools/swarm/register.hpp"
#include "tools/internal.hpp"

#include <array>

namespace orangutan::tools {

    namespace {
        constexpr std::array SWARM_TOOL_REGISTRARS = {
            register_team_create_tool,
            register_team_delete_tool,
        };
    } // namespace

    void register_swarm_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        register_contextual_tools(registry, tool_context, SWARM_TOOL_REGISTRARS);
    }

} // namespace orangutan::tools
