#pragma once

#include "core/tools/tool.hpp"

#include <string>
#include <string_view>

namespace orangutan::app {

struct SlashCommandReply {
    bool handled = false;
    std::string text;
};

[[nodiscard]]
std::string trim_copy(std::string_view input);

[[nodiscard]]
SlashCommandReply handle_registry_slash_command(const std::string &line, const ToolRegistry *tool_registry);

} // namespace orangutan::app
