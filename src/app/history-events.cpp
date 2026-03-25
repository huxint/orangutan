#include "app/history-events.hpp"

#include <algorithm>
#include "infra/format.hpp"
#include <unordered_map>

namespace orangutan::app {

namespace {

std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (lines.empty() && !text.empty()) {
        lines.push_back(text);
    }
    return lines;
}

json make_diff_line(const char *kind, const std::string &text) {
    return {
        {"kind", kind},
        {"text", text},
    };
}

json history_text_event(std::string_view role, const std::string &text) {
    return {
        {"type", "history_message"},
        {"role", role},
        {"text", text},
    };
}

} // namespace

json make_session_event_json(const char *type, const std::string &session_id, const std::string &action) {
    return {
        {"type", type},
        {"session_id", session_id},
        {"action", action},
    };
}

json build_edit_details(const ToolUseBlock &call) {
    if (call.name != "edit") {
        return nullptr;
    }

    const auto old_text = call.input.value("old_text", std::string{});
    const auto new_text = call.input.value("new_text", std::string{});
    const auto path = call.input.value("path", std::string{});
    const auto old_lines = split_lines(old_text);
    const auto new_lines = split_lines(new_text);

    size_t prefix = 0;
    while (prefix < old_lines.size() && prefix < new_lines.size() && old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }

    size_t suffix = 0;
    while (suffix < (old_lines.size() - prefix) && suffix < (new_lines.size() - prefix) && old_lines[old_lines.size() - 1 - suffix] == new_lines[new_lines.size() - 1 - suffix]) {
        ++suffix;
    }

    constexpr size_t context_lines = 3;
    const auto prefix_context_start = prefix > context_lines ? prefix - context_lines : 0;
    const auto prefix_context_count = prefix - prefix_context_start;
    const auto suffix_context_start = old_lines.size() - suffix;
    const auto suffix_context_count = std::min(suffix, context_lines);

    auto hunk_lines = json::array();
    for (size_t index = prefix_context_start; index < prefix; ++index) {
        hunk_lines.push_back(make_diff_line("context", old_lines[index]));
    }
    for (size_t index = prefix; index < old_lines.size() - suffix; ++index) {
        hunk_lines.push_back(make_diff_line("removal", old_lines[index]));
    }
    for (size_t index = prefix; index < new_lines.size() - suffix; ++index) {
        hunk_lines.push_back(make_diff_line("addition", new_lines[index]));
    }
    for (size_t index = suffix_context_start; index < suffix_context_start + suffix_context_count; ++index) {
        hunk_lines.push_back(make_diff_line("context", old_lines[index]));
    }

    const auto start_old = prefix_context_start + 1;
    const auto start_new = prefix_context_start + 1;
    const auto count_old = prefix_context_count + (old_lines.size() - prefix - suffix) + suffix_context_count;
    const auto count_new = prefix_context_count + (new_lines.size() - prefix - suffix) + suffix_context_count;
    const auto header = fmt::format("@@ -{},{} +{},{} @@", start_old, count_old, start_new, count_new);

    std::string unified;
    unified.append("--- a/");
    unified.append(path.empty() ? "before" : path);
    unified.push_back('\n');
    unified.append("+++ b/");
    unified.append(path.empty() ? "after" : path);
    unified.push_back('\n');
    append(unified, "{}", header);

    for (size_t index = prefix_context_start; index < prefix; ++index) {
        unified.push_back('\n');
        unified.push_back(' ');
        unified.append(old_lines[index]);
    }
    for (size_t index = prefix; index < old_lines.size() - suffix; ++index) {
        unified.push_back('\n');
        unified.push_back('-');
        unified.append(old_lines[index]);
    }
    for (size_t index = prefix; index < new_lines.size() - suffix; ++index) {
        unified.push_back('\n');
        unified.push_back('+');
        unified.append(new_lines[index]);
    }
    for (size_t index = suffix_context_start; index < suffix_context_start + suffix_context_count; ++index) {
        unified.push_back('\n');
        unified.push_back(' ');
        unified.append(old_lines[index]);
    }

    auto hunks = json::array();
    hunks.push_back({
        {"header", header},
        {"lines", hunk_lines},
    });

    return {
        {"type", "edit"},
        {"path", path},
        {"diff",
         {
             {"unified", unified},
             {"hunks", hunks},
         }},
    };
}

std::vector<json> build_session_history_events(const std::vector<Message> &history) {
    std::vector<json> events;
    events.push_back({{"type", "session_history_started"}});

    std::unordered_map<std::string, ToolUseBlock> tool_calls;
    for (const auto &message : history) {
        for (const auto &block : message.content) {
            if (const auto *text = std::get_if<TextBlock>(&block)) {
                events.push_back(history_text_event(magic_enum::enum_name(message.role), text->text));
                continue;
            }

            if (const auto *tool = std::get_if<ToolUseBlock>(&block)) {
                tool_calls.insert_or_assign(tool->id, *tool);
                events.push_back({
                    {"type", "tool_started"},
                    {"id", tool->id},
                    {"name", tool->name},
                    {"input", tool->input},
                });
                continue;
            }

            const auto *result = std::get_if<ToolResultBlock>(&block);
            if (result == nullptr) {
                continue;
            }

            const auto tool_it = tool_calls.find(result->tool_use_id);
            const auto tool_name = tool_it != tool_calls.end() ? tool_it->second.name : "tool";
            const auto tool_input = tool_it != tool_calls.end() ? tool_it->second.input : json::object();
            const auto details = tool_it != tool_calls.end() ? build_edit_details(tool_it->second) : json(nullptr);

            events.push_back({
                {"type", "tool_finished"},
                {"id", result->tool_use_id},
                {"name", tool_name},
                {"input", tool_input},
                {"output", result->content},
                {"is_error", result->is_error},
                {"details", details},
            });
        }
    }

    events.push_back({{"type", "session_history_finished"}});
    return events;
}

} // namespace orangutan::app
