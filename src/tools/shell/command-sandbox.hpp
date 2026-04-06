#pragma once

#include <string>
#include "types/base.hpp"

namespace orangutan::tools {

    enum class tool_sandbox_mode : base::u8 {
        isolated,
        workspace_write,
        disabled,
    };

    struct SandboxedCommand {
        std::string command;
        std::string working_dir;
    };

    [[nodiscard]]
    SandboxedCommand prepare_sandboxed_command(const std::string &command, const std::string &workspace_root, const std::string &working_dir, tool_sandbox_mode sandbox_mode);

} // namespace orangutan::tools
