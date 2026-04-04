#pragma once

#include <string>
#include <vector>

namespace orangutan::tools {
    struct ToolRuntimeContext;
}

namespace orangutan::coordinator {

    [[nodiscard]]
    bool is_coordinator_mode(const tools::ToolRuntimeContext *tool_context);

    [[nodiscard]]
    const std::vector<std::string> &get_coordinator_allowed_tools();

} // namespace orangutan::coordinator
