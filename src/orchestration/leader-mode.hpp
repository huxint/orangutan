#pragma once

#include "orchestration/types.hpp"
#include "tools/registry/tool-context.hpp"

namespace orangutan::orchestration {

    [[nodiscard]]
    inline bool is_leader_mode(const tools::ToolRuntimeContext *tool_context) {
        return tool_context != nullptr && is_leader(tool_context->role);
    }

} // namespace orangutan::orchestration
