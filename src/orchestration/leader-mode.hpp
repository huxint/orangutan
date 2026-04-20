#pragma once

namespace orangutan::tools {
    struct ToolRuntimeContext;
}

namespace orangutan::orchestration {

    [[nodiscard]]
    bool is_leader_mode(const tools::ToolRuntimeContext *tool_context);

} // namespace orangutan::orchestration
