#pragma once

#include "types/base.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace orangutan::tools {

    enum class tool_sandbox_mode : std::uint8_t {
        isolated,
        workspace_write,
        disabled,
    };

    struct SandboxedCommand {
        std::string command;
        std::filesystem::path working_dir;
    };

    [[nodiscard]]
    SandboxedCommand prepare_sandboxed_command(std::string_view command, const std::filesystem::path &workspace_root, const std::filesystem::path &working_dir,
                                               tool_sandbox_mode sandbox_mode);

} // namespace orangutan::tools
