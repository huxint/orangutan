#include "tools/coordinator/register.hpp"
#include "tools/registry/tool-context.hpp"

#include <array>
#include <string_view>

namespace orangutan::tools {

    namespace {
        using CoordinatorToolRegistrar = void (*)(ToolRegistry &, const ToolRuntimeContext *);

        struct CoordinatorToolSpec {
            std::string_view name;
            CoordinatorToolRegistrar register_tool;
        };

        constexpr std::array COORDINATOR_TOOL_SPECS = {
            CoordinatorToolSpec{.name = "agent_spawn", .register_tool = register_agent_spawn_tool},
            CoordinatorToolSpec{.name = "agent_send_message", .register_tool = register_agent_send_message_tool},
            CoordinatorToolSpec{.name = "agent_stop", .register_tool = register_agent_stop_tool},
        };
    } // namespace

    void register_coordinator_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }

        for (const auto &spec : COORDINATOR_TOOL_SPECS) {
            spec.register_tool(registry, tool_context);
        }
    }


} // namespace orangutan::tools
