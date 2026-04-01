#include "tools/subagent/register.hpp"

#include "tools/register.hpp"

namespace orangutan::tools::subagent {

    void register_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        register_builtin_subagent_tools(registry, tool_context);
    }

} // namespace orangutan::tools::subagent
