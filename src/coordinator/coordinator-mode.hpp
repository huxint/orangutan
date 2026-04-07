#pragma once

namespace orangutan::tools {
    struct ToolRuntimeContext;
}

namespace orangutan::coordinator {

    [[nodiscard]]
    bool is_coordinator_mode(const tools::ToolRuntimeContext *tool_context);

} // namespace orangutan::coordinator
