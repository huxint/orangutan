#include "tools/orchestration/register.hpp"
#include <array>

namespace orangutan::tools {

    namespace {
        constexpr std::array ORCHESTRATION_TOOL_REGISTRARS = {
            register_agent_spawn_tool,
            register_agent_send_message_tool,
            register_agent_stop_tool,
            register_team_create_tool,
            register_team_delete_tool,
        };
    } // namespace

    void register_orchestration_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }

        for (const auto registrar : ORCHESTRATION_TOOL_REGISTRARS) {
            registrar(registry, tool_context);
        }
    }

} // namespace orangutan::tools
