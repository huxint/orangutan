#pragma once

#include "tools/registry/tool.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace orangutan::cli {

    struct SlashCommandReply {
        bool handled = false;
        std::string text;
        std::optional<std::string> session_id;
    };

    enum class slash_command_surface : std::uint8_t {
        cli,
        channel,
        web,
    };

    using slash_reply_handler = std::function<SlashCommandReply()>;
    using slash_argument_reply_handler = std::function<SlashCommandReply(const std::string &)>;

    struct SharedSlashCommandContext {
        slash_command_surface surface = slash_command_surface::cli;
        slash_reply_handler help;
        slash_reply_handler new_session;
        slash_reply_handler export_session;
        slash_reply_handler compress;
        slash_reply_handler session;
        slash_reply_handler sessions;
        slash_reply_handler agent;
        slash_reply_handler status;
        slash_reply_handler agents;
        slash_argument_reply_handler resume;
        const ToolRegistry *tool_registry = nullptr;
    };

    [[nodiscard]]
    std::string render_slash_help_text(slash_command_surface surface);

    [[nodiscard]]
    SlashCommandReply dispatch_shared_slash_command(const std::string &line, const SharedSlashCommandContext &context);

    [[nodiscard]]
    SlashCommandReply handle_registry_slash_command(const std::string &line, const ToolRegistry *tool_registry);

} // namespace orangutan::cli
