#include "tools/shell/register.hpp"

#include "tools/background/background-completion.hpp"
#include "tools/internal.hpp"

namespace orangutan::tools::shell {

    void register_tools(ToolRegistry &registry, const std::string &workspace, const ToolRuntimeContext *tool_context, const ToolPermissionContext *permissions) {
        const auto completion_dispatcher = std::make_shared<BackgroundCompletionDispatcher>(tool_context);
        const auto process_manager = std::make_shared<BackgroundProcessManager>([completion_dispatcher](const BackgroundProcessCompletionEvent &event) {
            completion_dispatcher->dispatch(event);
        });
        register_shell_tool(registry, workspace, permissions, completion_dispatcher, process_manager);
        register_process_tools(registry, process_manager);
    }

} // namespace orangutan::tools::shell
