#include "tools/inbox/inbox-tool.hpp"
#include "tools/registry/tool-registry.hpp"

namespace orangutan::tools::inbox {

    void register_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        register_inbox_tool(registry, tool_context);
    }

} // namespace orangutan::tools::inbox
