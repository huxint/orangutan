#include "cli/history-events.hpp"

#include <algorithm>
#include <unordered_map>

#include "utils/format.hpp"
#include "utils/string.hpp"

namespace orangutan::cli {

    namespace {

        std::vector<std::string> split_diff_lines(std::string_view text) {
            auto lines = utils::split_lines(text);
            for (auto &line : lines) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
            }
            return lines;
        }

        nlohmann::json make_diff_line(const char *kind, const std::string &text) {
            return {
                {"kind", kind},
                {"text", text},
            };
        }

        nlohmann::json history_text_event(std::string_view role, const std::string &text) {
            return {
                {"type", "history_message"},
                {"role", role},
                {"text", text},
            };
        }

    } // namespace

    nlohmann::json make_session_event_json(const char *type, const std::string &session_id, const std::string &action) {
        return {
            {"type", type},
            {"session_id", session_id},
            {"action", action},
        };
    }

    nlohmann::json build_edit_details(const ToolUse &call) {
        if (call.name != "edit") {
            return nullptr;
        }

        const auto old_text = call.input.value("old_text", std::string{});
        const auto new_text = call.input.value("new_text", std::string{});
        const auto path = call.input.value("path", std::string{});
        const auto old_lines = split_diff_lines(old_text);
        const auto new_lines = split_diff_lines(new_text);

        std::size_t prefix = 0;
        while (prefix < old_lines.size() && prefix < new_lines.size() && old_lines[prefix] == new_lines[prefix]) {
            ++prefix;
        }

        std::size_t suffix = 0;
        while (suffix < (old_lines.size() - prefix) && suffix < (new_lines.size() - prefix) &&
               old_lines[old_lines.size() - 1 - suffix] == new_lines[new_lines.size() - 1 - suffix]) {
            ++suffix;
        }

        constexpr std::size_t CONTEXT_LINES = 3;
        const auto prefix_context_start = prefix > CONTEXT_LINES ? prefix - CONTEXT_LINES : 0;
        const auto prefix_context_count = prefix - prefix_context_start;
        const auto suffix_context_start = old_lines.size() - suffix;
        const auto suffix_context_count = std::min(suffix, CONTEXT_LINES);

        auto hunk_lines = nlohmann::json::array();
        for (std::size_t index = prefix_context_start; index < prefix; ++index) {
            hunk_lines.push_back(make_diff_line("context", old_lines[index]));
        }
        for (std::size_t index = prefix; index < old_lines.size() - suffix; ++index) {
            hunk_lines.push_back(make_diff_line("removal", old_lines[index]));
        }
        for (std::size_t index = prefix; index < new_lines.size() - suffix; ++index) {
            hunk_lines.push_back(make_diff_line("addition", new_lines[index]));
        }
        for (std::size_t index = suffix_context_start; index < suffix_context_start + suffix_context_count; ++index) {
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
        utils::format_to(unified, "{}", header);

        for (std::size_t index = prefix_context_start; index < prefix; ++index) {
            unified.push_back('\n');
            unified.push_back(' ');
            unified.append(old_lines[index]);
        }
        for (std::size_t index = prefix; index < old_lines.size() - suffix; ++index) {
            unified.push_back('\n');
            unified.push_back('-');
            unified.append(old_lines[index]);
        }
        for (std::size_t index = prefix; index < new_lines.size() - suffix; ++index) {
            unified.push_back('\n');
            unified.push_back('+');
            unified.append(new_lines[index]);
        }
        for (std::size_t index = suffix_context_start; index < suffix_context_start + suffix_context_count; ++index) {
            unified.push_back('\n');
            unified.push_back(' ');
            unified.append(old_lines[index]);
        }

        auto hunks = nlohmann::json::array();
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

    std::vector<nlohmann::json> build_session_history_events(const std::vector<Message> &history) {
        std::vector<nlohmann::json> events;
        events.push_back({{"type", "session_history_started"}});

        std::unordered_map<std::string, ToolUse> tool_calls;
        for (const auto &message : history) {
            for (const auto &block : message) {
                if (const auto *text = std::get_if<Text>(&block)) {
                    events.push_back(history_text_event(magic_enum::enum_name(message.role()), text->text));
                    continue;
                }

                if (const auto *tool = std::get_if<ToolUse>(&block)) {
                    tool_calls.insert_or_assign(tool->id, *tool);
                    events.push_back({
                        {"type", "tool_started"},
                        {"id", tool->id},
                        {"name", tool->name},
                        {"input", tool->input},
                    });
                    continue;
                }

                const auto *result = std::get_if<ToolResult>(&block);
                if (result == nullptr) {
                    continue;
                }

                const auto tool_it = tool_calls.find(result->tool_use_id);
                const auto tool_name = tool_it != tool_calls.end() ? tool_it->second.name : "tool";
                const auto tool_input = tool_it != tool_calls.end() ? tool_it->second.input : nlohmann::json::object();
                const auto details = tool_it != tool_calls.end() ? build_edit_details(tool_it->second) : nlohmann::json(nullptr);

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

} // namespace orangutan::cli
