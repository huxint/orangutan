#pragma once

#include "tools/registry/permissions.hpp"

#include <string>

namespace orangutan::tools {

    struct SandboxedCommand {
        std::string command;
        std::string working_dir;
    };

    [[nodiscard]]
    SandboxedCommand prepare_sandboxed_command(const std::string &command, const std::string &workspace_root, const std::string &working_dir, ToolSandboxMode sandbox_mode);

} // namespace orangutan::tools
