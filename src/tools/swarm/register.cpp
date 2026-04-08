#include "tools/swarm/register.hpp"
#include <array>

namespace orangutan::tools {

    namespace {
        constexpr std::array SWARM_TOOL_REGISTRARS = {
            register_team_create_tool,
            register_team_delete_tool,
        };
    } // namespace

    void register_swarm_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }

        for (const auto registrar : SWARM_TOOL_REGISTRARS) {
            registrar(registry, tool_context);
        }
    }

} // namespace orangutan::tools
