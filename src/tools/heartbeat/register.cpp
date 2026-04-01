#include "tools/heartbeat/register.hpp"

#include "tools/heartbeat/heartbeat-tool.hpp"

namespace orangutan::tools::heartbeat {

    void register_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        register_heartbeat_tool(registry, tool_context);
    }

} // namespace orangutan::tools::heartbeat
