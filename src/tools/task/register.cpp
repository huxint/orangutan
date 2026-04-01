#include "tools/task/register.hpp"

#include "tools/task/task-tool.hpp"

namespace orangutan::tools::task {

    void register_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        register_task_tool(registry, tool_context);
    }

} // namespace orangutan::tools::task
