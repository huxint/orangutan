#include "app/slash-commands.hpp"

#include "infra/string.hpp"
#include <array>
#include "infra/format.hpp"

namespace orangutan::app {

    namespace {

        enum class slash_argument_mode {
            none,
            optional,
            required,
        };

        enum class slash_command_surface_mask : base::u8 {
            none = 0,
            cli = 1 << 0,
            channel = 1 << 1,
            web = 1 << 2,
            all = (1 << 0) | (1 << 1) | (1 << 2),
        };

        struct SlashCommandDescription {
            constexpr SlashCommandDescription() = default;
            constexpr SlashCommandDescription(std::string_view cli_value, std::string_view channel_value, std::string_view web_value)
            : cli(cli_value),
              channel(channel_value),
              web(web_value) {}

            std::string_view cli;
            std::string_view channel;
            std::string_view web;
        };

        struct SlashCommandDefinition {
            constexpr SlashCommandDefinition() = default;
            constexpr SlashCommandDefinition(std::string_view name_value, std::string_view usage_value, slash_argument_mode argument_mode_value,
                                             slash_command_surface_mask surfaces_value, SlashCommandDescription description_value)
            : name(name_value),
              usage(usage_value),
              argument_mode(argument_mode_value),
              surfaces(surfaces_value),
              description(description_value) {}

            std::string_view name;
            std::string_view usage;
            slash_argument_mode argument_mode = slash_argument_mode::none;
            slash_command_surface_mask surfaces = slash_command_surface_mask::all;
            SlashCommandDescription description;
        };

        struct ParsedSlashCommand {
            ParsedSlashCommand() = default;
            ParsedSlashCommand(std::string name_value, std::string args_value, bool valid_value)
            : name(std::move(name_value)),
              args(std::move(args_value)),
              valid(valid_value) {}

            std::string name;
            std::string args;
            bool valid = false;
        };

        using shared_slash_executor = SlashCommandReply (*)(std::string_view args, const SharedSlashCommandContext &context);
        struct SharedSlashCommandSpec {
            SlashCommandDefinition definition;
            shared_slash_executor execute = nullptr;
        };

        using registry_slash_executor = SlashCommandReply (*)(std::string_view args, const ToolRegistry *tool_registry);
        struct RegistrySlashCommandSpec {
            SlashCommandDefinition definition;
            registry_slash_executor execute = nullptr;
        };

        struct HelpOnlySlashCommandSpec {
            SlashCommandDefinition definition;
        };

        constexpr slash_command_surface_mask operator|(slash_command_surface_mask lhs, slash_command_surface_mask rhs) {
            return static_cast<slash_command_surface_mask>(static_cast<base::u8>(lhs) | static_cast<base::u8>(rhs));
        }

        constexpr bool supports_surface(slash_command_surface_mask mask, slash_command_surface surface) {
            const auto bit = [surface]() constexpr {
                switch (surface) {
                    case slash_command_surface::cli:
                        return slash_command_surface_mask::cli;
                    case slash_command_surface::channel:
                        return slash_command_surface_mask::channel;
                    case slash_command_surface::web:
                        return slash_command_surface_mask::web;
                }
                return slash_command_surface_mask::none;
            }();
            return (static_cast<base::u8>(mask) & static_cast<base::u8>(bit)) != 0;
        }

        std::string_view description_for_surface(const SlashCommandDescription &description, slash_command_surface surface) {
            switch (surface) {
                case slash_command_surface::cli:
                    return description.cli;
                case slash_command_surface::channel:
                    return description.channel;
                case slash_command_surface::web:
                    return description.web;
            }
            return {};
        }

        constexpr SlashCommandDescription make_description(std::string_view cli, std::string_view channel, std::string_view web) {
            return {cli, channel, web};
        }

        constexpr SlashCommandDefinition make_definition(std::string_view name, std::string_view usage, slash_argument_mode argument_mode, slash_command_surface_mask surfaces,
                                                         SlashCommandDescription description) {
            return {name, usage, argument_mode, surfaces, description};
        }

        constexpr SharedSlashCommandSpec make_shared_spec(SlashCommandDefinition definition, shared_slash_executor execute) {
            return {.definition = definition, .execute = execute};
        }

        constexpr RegistrySlashCommandSpec make_registry_spec(SlashCommandDefinition definition, registry_slash_executor execute) {
            return {.definition = definition, .execute = execute};
        }

        constexpr HelpOnlySlashCommandSpec make_help_only_spec(SlashCommandDefinition definition) {
            return {.definition = definition};
        }

        ParsedSlashCommand parse_command(std::string_view input) {
            if (input.empty()) {
                return {};
            }

            const auto trimmed = utils::trim_copy(input);
            if (trimmed.empty()) {
                return {};
            }

            const auto separator = trimmed.find_first_of(" \t");
            if (separator == std::string::npos) {
                return {static_cast<std::string>(trimmed), {}, true};
            }

            return {static_cast<std::string>(trimmed.substr(0, separator)), static_cast<std::string>(utils::trim_copy(trimmed.substr(separator + 1))), true};
        }

        ParsedSlashCommand parse_slash_command(const std::string &line) {
            if (!line.starts_with('/')) {
                return {};
            }

            return parse_command(std::string_view(line).substr(1));
        }

        bool matches_command_name(const ParsedSlashCommand &command, const SlashCommandDefinition &definition) {
            return command.valid && command.name == definition.name;
        }

        SlashCommandReply usage_reply(std::string_view usage) {
            std::string text{"Usage: "};
            text += usage;
            return {.handled = true, .text = std::move(text)};
        }

        template <typename Context, typename Specs>
        SlashCommandReply dispatch_command_table(const ParsedSlashCommand &command, const Specs &specs, const Context &context) {
            for (const auto &spec : specs) {
                if (!matches_command_name(command, spec.definition) || spec.execute == nullptr) {
                    continue;
                }
                if (spec.definition.argument_mode == slash_argument_mode::none && !command.args.empty()) {
                    return {};
                }
                if (spec.definition.argument_mode == slash_argument_mode::required && command.args.empty()) {
                    return usage_reply(spec.definition.usage);
                }
                return spec.execute(command.args, context);
            }
            return {};
        }

        SlashCommandReply invoke(const slash_reply_handler &callback) {
            return callback ? callback() : SlashCommandReply{};
        }

        SlashCommandReply invoke_with_arg(const slash_argument_reply_handler &callback, std::string_view arg) {
            return callback ? callback(std::string(arg)) : SlashCommandReply{};
        }

        template <auto Member>
        SlashCommandReply invoke_no_arg(std::string_view /*args*/, const SharedSlashCommandContext &context) {
            return invoke(context.*Member);
        }

        template <auto Member>
        SlashCommandReply invoke_required_arg(std::string_view args, const SharedSlashCommandContext &context) {
            return invoke_with_arg(context.*Member, args);
        }

        std::string wrap_slash_reply(std::string_view title, std::string_view emoji, const std::string &text) {
            std::string out;
            append(out, "## {}\n", title);

            if (text.empty()) {
                append(out, "- {} No output.", emoji);
                return out;
            }

            if (text.starts_with("Error: ")) {
                append(out, "- ⚠️ {}", text.substr(7));
                return out;
            }

            if (text.starts_with("Usage: ")) {
                append(out, "- ℹ️ `{}`", text);
                return out;
            }

            if (text.starts_with("- ")) {
                out += text;
                return out;
            }

            append(out, "- {} {}", emoji, text);
            return out;
        }

        SlashCommandReply execute_registry_command(const ToolRegistry *tool_registry, std::string_view tool_name, const nlohmann::json &input, std::string_view tool_use_id) {
            if (tool_registry == nullptr) {
                return {.handled = true, .text = "No tool registry available."};
            }

            const auto result = tool_registry->execute(ToolUse(std::string(tool_use_id), std::string(tool_name), input));
            return {.handled = true, .text = result.content};
        }

        constexpr auto k_shared_slash_commands = std::array{
            make_shared_spec(make_definition("help", "/help", slash_argument_mode::none, slash_command_surface_mask::all,
                                             make_description("show this help", "show this help", "show this help")),
                             invoke_no_arg<&SharedSlashCommandContext::help>),
            make_shared_spec(make_definition("new", "/new", slash_argument_mode::none, slash_command_surface_mask::all,
                                             make_description("save current session and start a new one", "start a new session", "start a new chat session")),
                             invoke_no_arg<&SharedSlashCommandContext::new_session>),
            make_shared_spec(make_definition("export", "/export", slash_argument_mode::none, slash_command_surface_mask::all,
                                             make_description("export the current session to the workspace", "export the current session to the workspace",
                                                              "export the current session to the workspace")),
                             invoke_no_arg<&SharedSlashCommandContext::export_session>),
            make_shared_spec(make_definition("compress", "/compress", slash_argument_mode::none, slash_command_surface_mask::all,
                                             make_description("summarize older history and keep recent messages verbatim", "summarize older history",
                                                              "summarize older history for the current session")),
                             invoke_no_arg<&SharedSlashCommandContext::compress>),
            make_shared_spec(make_definition("session", "/session", slash_argument_mode::none, slash_command_surface_mask::all,
                                             make_description("show the current session id", "show the current session id", "show the current session id")),
                             invoke_no_arg<&SharedSlashCommandContext::session>),
            make_shared_spec(make_definition("sessions", "/sessions", slash_argument_mode::none, slash_command_surface_mask::all,
                                             make_description("list saved sessions for the current agent scope", "list saved sessions in this scope",
                                                              "list saved sessions for the current agent")),
                             invoke_no_arg<&SharedSlashCommandContext::sessions>),
            make_shared_spec(make_definition("agent", "/agent", slash_argument_mode::none, slash_command_surface_mask::all,
                                             make_description("show the current agent", "show the current agent", "show the current agent")),
                             invoke_no_arg<&SharedSlashCommandContext::agent>),
            make_shared_spec(
                make_definition("status", "/status", slash_argument_mode::none, slash_command_surface_mask::all,
                                make_description("show active model and runtime status", "show active model and runtime status", "show active model and runtime status")),
                invoke_no_arg<&SharedSlashCommandContext::status>),
            make_shared_spec(make_definition("agents", "/agents", slash_argument_mode::none, slash_command_surface_mask::all,
                                             make_description("list configured agents", "list configured agents", "list configured agents")),
                             invoke_no_arg<&SharedSlashCommandContext::agents>),
            make_shared_spec(make_definition("resume", "/resume <session-id>", slash_argument_mode::required, slash_command_surface_mask::all,
                                             make_description("resume a saved session; supports `latest`", "resume a saved session or use `latest`",
                                                              "switch to a saved session or use `latest`")),
                             invoke_required_arg<&SharedSlashCommandContext::resume>),
        };

        SlashCommandReply handle_tasks_command(std::string_view args, const ToolRegistry *tool_registry) {
            if (args.empty()) {
                auto reply = execute_registry_command(tool_registry, "task", {{"op", "list"}}, "slash-task-list");
                if (reply.handled) {
                    reply.text = wrap_slash_reply("Tasks", "🗓️", reply.text);
                }
                return reply;
            }

            const auto remainder = utils::trim_copy(args);
            if (remainder.starts_with("run ")) {
                const auto id = static_cast<std::string>(utils::trim_copy(remainder.substr(4)));
                if (id.empty()) {
                    return {.handled = true, .text = wrap_slash_reply("Tasks", "🗓️", "Usage: /tasks run <id>")};
                }
                auto reply = execute_registry_command(tool_registry, "task", {{"op", "run"}, {"id", id}}, "slash-task-run");
                if (reply.handled) {
                    reply.text = wrap_slash_reply("Tasks", "🗓️", reply.text);
                }
                return reply;
            }
            if (remainder.starts_with("remove ")) {
                const auto id = static_cast<std::string>(utils::trim_copy(remainder.substr(7)));
                if (id.empty()) {
                    return {.handled = true, .text = wrap_slash_reply("Tasks", "🗓️", "Usage: /tasks remove <id>")};
                }
                auto reply = execute_registry_command(tool_registry, "task", {{"op", "remove"}, {"id", id}}, "slash-task-remove");
                if (reply.handled) {
                    reply.text = wrap_slash_reply("Tasks", "🗓️", reply.text);
                }
                return reply;
            }

            return {.handled = true, .text = wrap_slash_reply("Tasks", "🗓️", "Usage: /tasks | /tasks run <id> | /tasks remove <id>")};
        }

        SlashCommandReply handle_heartbeats_command(std::string_view args, const ToolRegistry *tool_registry) {
            if (args.empty()) {
                auto reply = execute_registry_command(tool_registry, "heartbeat", {{"op", "list"}}, "slash-heartbeat-list");
                if (reply.handled) {
                    reply.text = wrap_slash_reply("Heartbeats", "💓", reply.text);
                }
                return reply;
            }

            const auto remainder = utils::trim_copy(args);
            const auto run_action = [&](std::string_view action, std::string_view op, std::string_view tool_use_id) -> SlashCommandReply {
                if (!remainder.starts_with(action)) {
                    return {};
                }
                const auto id = static_cast<std::string>(utils::trim_copy(remainder.substr(action.size())));
                if (id.empty()) {
                    return {.handled = true, .text = wrap_slash_reply("Heartbeats", "💓", "Usage: /heartbeats " + std::string(op) + " <id>")};
                }
                auto reply = execute_registry_command(tool_registry, "heartbeat", {{"op", std::string(op)}, {"id", id}}, tool_use_id);
                if (reply.handled) {
                    reply.text = wrap_slash_reply("Heartbeats", "💓", reply.text);
                }
                return reply;
            };

            if (auto reply = run_action("run ", "run", "slash-heartbeat-run"); reply.handled) {
                return reply;
            }
            if (auto reply = run_action("pause ", "pause", "slash-heartbeat-pause"); reply.handled) {
                return reply;
            }
            if (auto reply = run_action("resume ", "resume", "slash-heartbeat-resume"); reply.handled) {
                return reply;
            }
            if (auto reply = run_action("remove ", "remove", "slash-heartbeat-remove"); reply.handled) {
                return reply;
            }

            return {.handled = true,
                    .text = wrap_slash_reply("Heartbeats", "💓",
                                             "Usage: /heartbeats | /heartbeats run <id> | /heartbeats pause <id> | /heartbeats resume <id> | /heartbeats remove <id>")};
        }

        SlashCommandReply handle_inbox_command(std::string_view args, const ToolRegistry *tool_registry) {
            if (args.empty()) {
                auto reply = execute_registry_command(tool_registry, "inbox", {{"op", "list"}}, "slash-inbox-list");
                if (reply.handled) {
                    reply.text = wrap_slash_reply("Inbox", "📥", reply.text);
                }
                return reply;
            }

            const auto remainder = utils::trim_copy(args);
            if (remainder == "clear") {
                auto reply = execute_registry_command(tool_registry, "inbox", {{"op", "clear"}}, "slash-inbox-clear");
                if (reply.handled) {
                    reply.text = wrap_slash_reply("Inbox", "📥", reply.text);
                }
                return reply;
            }

            if (remainder.starts_with("ack ")) {
                const auto id = static_cast<std::string>(utils::trim_copy(remainder.substr(4)));
                if (id.empty()) {
                    return {.handled = true, .text = wrap_slash_reply("Inbox", "📥", "Usage: /inbox ack <id>")};
                }
                auto reply = execute_registry_command(tool_registry, "inbox", {{"op", "ack"}, {"id", id}}, "slash-inbox-ack");
                if (reply.handled) {
                    reply.text = wrap_slash_reply("Inbox", "📥", reply.text);
                }
                return reply;
            }

            return {.handled = true, .text = wrap_slash_reply("Inbox", "📥", "Usage: /inbox | /inbox ack <id> | /inbox clear")};
        }

        constexpr auto k_registry_slash_commands = std::array{
            make_registry_spec(make_definition("tasks", "/tasks", slash_argument_mode::optional, slash_command_surface_mask::all,
                                               make_description("list tasks or run `/tasks run <id>`", "list tasks, `/tasks run <id>`, or `/tasks remove <id>`",
                                                                "list tasks, `/tasks run <id>`, or `/tasks remove <id>`")),
                               handle_tasks_command),
            make_registry_spec(make_definition("heartbeats", "/heartbeats", slash_argument_mode::optional, slash_command_surface_mask::all,
                                               make_description("list heartbeats or run `/heartbeats pause <id>`", "list heartbeats or run `/heartbeats pause <id>`",
                                                                "list heartbeats or run `/heartbeats pause <id>`")),
                               handle_heartbeats_command),
            make_registry_spec(make_definition("inbox", "/inbox", slash_argument_mode::optional, slash_command_surface_mask::all,
                                               make_description("list inbox items, `/inbox ack <id>`, or `/inbox clear`", "list inbox items, `/inbox ack <id>`, or `/inbox clear`",
                                                                "list inbox items, `/inbox ack <id>`, or `/inbox clear`")),
                               handle_inbox_command),
        };

        constexpr auto k_cli_only_help_commands = std::array{
            make_help_only_spec(
                make_definition("clear", "/clear", slash_argument_mode::none, slash_command_surface_mask::cli, make_description("clear conversation history", "", ""))),
            make_help_only_spec(
                make_definition("tools", "/tools", slash_argument_mode::none, slash_command_surface_mask::cli, make_description("list all registered tools", "", ""))),
            make_help_only_spec(make_definition("skills", "/skills", slash_argument_mode::none, slash_command_surface_mask::cli, make_description("list loaded skills", "", ""))),
            make_help_only_spec(make_definition("multi", "/multi", slash_argument_mode::none, slash_command_surface_mask::cli,
                                                make_description("enter multi-line mode and finish with an empty line", "", ""))),
            make_help_only_spec(make_definition("save", "/save", slash_argument_mode::none, slash_command_surface_mask::cli, make_description("save current session", "", ""))),
            make_help_only_spec(make_definition("quit", "/quit", slash_argument_mode::none, slash_command_surface_mask::cli, make_description("exit", "", ""))),
        };

        template <typename Specs>
        void append_help_lines(std::string &out, const Specs &specs, slash_command_surface surface) {
            for (const auto &spec : specs) {
                if (!supports_surface(spec.definition.surfaces, surface)) {
                    continue;
                }
                const auto description = description_for_surface(spec.definition.description, surface);
                if (description.empty()) {
                    continue;
                }
                append(out, "- `{}` - {}\n", spec.definition.usage, description);
            }
        }

    } // namespace

    std::string render_slash_help_text(slash_command_surface surface) {
        std::string out = "## Commands\n";
        append_help_lines(out, k_shared_slash_commands, surface);
        append_help_lines(out, k_registry_slash_commands, surface);
        append_help_lines(out, k_cli_only_help_commands, surface);
        if (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        return out;
    }

    SlashCommandReply dispatch_shared_slash_command(const std::string &line, const SharedSlashCommandContext &context) {
        const auto command = parse_slash_command(line);
        if (const auto reply = dispatch_command_table(command, k_shared_slash_commands, context); reply.handled) {
            return reply;
        }
        if (context.tool_registry != nullptr) {
            if (const auto reply = handle_registry_slash_command(line, context.tool_registry); reply.handled) {
                return reply;
            }
        }
        return {};
    }

    SlashCommandReply handle_registry_slash_command(const std::string &line, const ToolRegistry *tool_registry) {
        const auto command = parse_slash_command(line);
        return dispatch_command_table(command, k_registry_slash_commands, tool_registry);
    }

} // namespace orangutan::app
