#include "app/slash-commands.hpp"

#include <sstream>

namespace orangutan::app {

std::string trim_copy(std::string_view input) {
    const auto begin = input.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = input.find_last_not_of(" \t");
    return std::string(input.substr(begin, end - begin + 1));
}

namespace {

std::string wrap_slash_reply(std::string_view title, std::string_view emoji, std::string text) {
    std::ostringstream out;
    out << "## " << title << '\n';

    if (text.empty()) {
        out << "- " << emoji << " No output.";
        return out.str();
    }

    if (text.starts_with("Error: ")) {
        out << "- ⚠️ " << text.substr(7);
        return out.str();
    }

    if (text.starts_with("Usage: ")) {
        out << "- ℹ️ `" << text << '`';
        return out.str();
    }

    if (text.starts_with("- ")) {
        out << text;
        return out.str();
    }

    out << "- " << emoji << ' ' << text;
    return out.str();
}

SlashCommandReply execute_registry_command(const ToolRegistry *tool_registry, std::string_view tool_name, const json &input, std::string_view tool_use_id) {
    if (tool_registry == nullptr) {
        return {.handled = true, .text = "No tool registry available."};
    }

    const auto result = tool_registry->execute(ToolUseBlock{
        .id = std::string(tool_use_id),
        .name = std::string(tool_name),
        .input = input,
    });
    return {.handled = true, .text = result.content};
}

SlashCommandReply handle_tasks_command(const std::string &line, const ToolRegistry *tool_registry) {
    if (line == "/tasks") {
        auto reply = execute_registry_command(tool_registry, "task", {{"op", "list"}}, "slash-task-list");
        if (reply.handled) {
            reply.text = wrap_slash_reply("Tasks", "🗓️", std::move(reply.text));
        }
        return reply;
    }
    if (!line.starts_with("/tasks ")) {
        return {};
    }

    const auto remainder = trim_copy(std::string_view(line).substr(7));
    if (remainder.starts_with("run ")) {
        const auto id = trim_copy(std::string_view(remainder).substr(4));
        if (id.empty()) {
            return {.handled = true, .text = wrap_slash_reply("Tasks", "🗓️", "Usage: /tasks run <id>")};
        }
        auto reply = execute_registry_command(tool_registry, "task", {{"op", "run"}, {"id", id}}, "slash-task-run");
        if (reply.handled) {
            reply.text = wrap_slash_reply("Tasks", "🗓️", std::move(reply.text));
        }
        return reply;
    }
    if (remainder.starts_with("remove ")) {
        const auto id = trim_copy(std::string_view(remainder).substr(7));
        if (id.empty()) {
            return {.handled = true, .text = wrap_slash_reply("Tasks", "🗓️", "Usage: /tasks remove <id>")};
        }
        auto reply = execute_registry_command(tool_registry, "task", {{"op", "remove"}, {"id", id}}, "slash-task-remove");
        if (reply.handled) {
            reply.text = wrap_slash_reply("Tasks", "🗓️", std::move(reply.text));
        }
        return reply;
    }

    return {.handled = true, .text = wrap_slash_reply("Tasks", "🗓️", "Usage: /tasks | /tasks run <id> | /tasks remove <id>")};
}

SlashCommandReply handle_heartbeats_command(const std::string &line, const ToolRegistry *tool_registry) {
    if (line == "/heartbeats") {
        auto reply = execute_registry_command(tool_registry, "heartbeat", {{"op", "list"}}, "slash-heartbeat-list");
        if (reply.handled) {
            reply.text = wrap_slash_reply("Heartbeats", "💓", std::move(reply.text));
        }
        return reply;
    }
    if (!line.starts_with("/heartbeats ")) {
        return {};
    }

    const auto remainder = trim_copy(std::string_view(line).substr(12));
    const auto run_action = [&](std::string_view action, std::string_view op, std::string_view tool_use_id) -> SlashCommandReply {
        if (!remainder.starts_with(action)) {
            return {};
        }
        const auto id = trim_copy(std::string_view(remainder).substr(action.size()));
        if (id.empty()) {
            return {.handled = true, .text = wrap_slash_reply("Heartbeats", "💓", "Usage: /heartbeats " + std::string(op) + " <id>")};
        }
        auto reply = execute_registry_command(tool_registry, "heartbeat", {{"op", std::string(op)}, {"id", id}}, tool_use_id);
        if (reply.handled) {
            reply.text = wrap_slash_reply("Heartbeats", "💓", std::move(reply.text));
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
            .text = wrap_slash_reply("Heartbeats", "💓", "Usage: /heartbeats | /heartbeats run <id> | /heartbeats pause <id> | /heartbeats resume <id> | /heartbeats remove <id>")};
}

SlashCommandReply handle_inbox_command(const std::string &line, const ToolRegistry *tool_registry) {
    if (line == "/inbox") {
        auto reply = execute_registry_command(tool_registry, "inbox", {{"op", "list"}}, "slash-inbox-list");
        if (reply.handled) {
            reply.text = wrap_slash_reply("Inbox", "📥", std::move(reply.text));
        }
        return reply;
    }
    if (line == "/inbox clear") {
        auto reply = execute_registry_command(tool_registry, "inbox", {{"op", "clear"}}, "slash-inbox-clear");
        if (reply.handled) {
            reply.text = wrap_slash_reply("Inbox", "📥", std::move(reply.text));
        }
        return reply;
    }
    if (!line.starts_with("/inbox ")) {
        return {};
    }

    const auto remainder = trim_copy(std::string_view(line).substr(7));
    if (remainder.starts_with("ack ")) {
        const auto id = trim_copy(std::string_view(remainder).substr(4));
        if (id.empty()) {
            return {.handled = true, .text = wrap_slash_reply("Inbox", "📥", "Usage: /inbox ack <id>")};
        }
        auto reply = execute_registry_command(tool_registry, "inbox", {{"op", "ack"}, {"id", id}}, "slash-inbox-ack");
        if (reply.handled) {
            reply.text = wrap_slash_reply("Inbox", "📥", std::move(reply.text));
        }
        return reply;
    }

    return {.handled = true, .text = wrap_slash_reply("Inbox", "📥", "Usage: /inbox | /inbox ack <id> | /inbox clear")};
}

} // namespace

SlashCommandReply handle_registry_slash_command(const std::string &line, const ToolRegistry *tool_registry) {
    if (auto reply = handle_tasks_command(line, tool_registry); reply.handled) {
        return reply;
    }
    if (auto reply = handle_heartbeats_command(line, tool_registry); reply.handled) {
        return reply;
    }
    if (auto reply = handle_inbox_command(line, tool_registry); reply.handled) {
        return reply;
    }
    return {};
}

} // namespace orangutan::app
