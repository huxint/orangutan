#pragma once

#include <algorithm>
#include <cstdio>
#include <charconv>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/fmt/bundled/color.h>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "agent/agent-loop.hpp"
#include "hooks/hook-manager.hpp"
#include "skills/skill-loader.hpp"
#include "utils/format.hpp"
#include "utils/sender-utils.hpp"
#include "utils/string.hpp"

namespace orangutan::agent::detail {

    inline constexpr int LOOP_DETECTION_THRESHOLD = 3;
    inline constexpr int LOOP_ABORT_THRESHOLD = 5;

    enum class loop_status : base::u8 {
        ok,
        warning,
        abort,
    };

    struct ToolCallSignature {
        std::string name;
        std::size_t input_hash;

        bool operator==(const ToolCallSignature &other) const = default;
    };

    struct SignatureHash {
        std::size_t operator()(const ToolCallSignature &sig) const {
            const auto name_hash = std::hash<std::string>{}(sig.name);
            return name_hash ^ (sig.input_hash << 1);
        }
    };

    using ToolCallCounts = std::unordered_map<ToolCallSignature, int, SignatureHash>;

    inline void append_path_field(std::vector<std::filesystem::path> &paths, const nlohmann::json &input, std::string_view key) {
        const auto it = input.find(std::string(key));
        if (it == input.end()) {
            return;
        }
        if (it->is_string()) {
            paths.emplace_back(it->get<std::string>());
        }
    }

    inline void append_paths_field(std::vector<std::filesystem::path> &paths, const nlohmann::json &input, std::string_view key) {
        const auto it = input.find(std::string(key));
        if (it == input.end() || !it->is_array()) {
            return;
        }

        for (const auto &entry : *it) {
            if (entry.is_string()) {
                paths.emplace_back(entry.get<std::string>());
            }
        }
    }

    [[nodiscard]]
    inline std::vector<std::filesystem::path> parse_patch_paths(std::string_view patch) {
        std::vector<std::filesystem::path> paths;
        for (const auto &line : utils::split_lines(patch)) {
            constexpr std::string_view UPDATE_HEADER = "*** Update File: ";
            constexpr std::string_view ADD_HEADER = "*** Add File: ";
            constexpr std::string_view DELETE_HEADER = "*** Delete File: ";

            std::string_view path_text;
            if (line.starts_with(UPDATE_HEADER)) {
                path_text = std::string_view(line).substr(UPDATE_HEADER.size());
            } else if (line.starts_with(ADD_HEADER)) {
                path_text = std::string_view(line).substr(ADD_HEADER.size());
            } else if (line.starts_with(DELETE_HEADER)) {
                path_text = std::string_view(line).substr(DELETE_HEADER.size());
            } else {
                continue;
            }

            while (!path_text.empty() && (path_text.back() == ' ' || path_text.back() == '\t' || path_text.back() == '\r')) {
                path_text.remove_suffix(1);
            }
            if (!path_text.empty()) {
                paths.emplace_back(path_text);
            }
        }
        return paths;
    }

    [[nodiscard]]
    inline std::vector<std::filesystem::path> touched_paths_for_tool_call(const ToolUse &call) {
        std::vector<std::filesystem::path> paths;
        if (call.name == "read") {
            append_path_field(paths, call.input, "path");
            append_paths_field(paths, call.input, "paths");
            return paths;
        }

        if (call.name == "write") {
            append_path_field(paths, call.input, "path");
            return paths;
        }

        if (call.name == "edit") {
            append_path_field(paths, call.input, "path");
            if (!paths.empty()) {
                return paths;
            }

            const auto patch_it = call.input.find("patch");
            if (patch_it != call.input.end() && patch_it->is_string()) {
                return parse_patch_paths(patch_it->get<std::string>());
            }

            append_paths_field(paths, call.input, "paths");
            if (!paths.empty()) {
                return paths;
            }

            const auto edits_it = call.input.find("edits");
            if (edits_it != call.input.end() && edits_it->is_array()) {
                for (const auto &edit : *edits_it) {
                    if (!edit.is_object()) {
                        continue;
                    }
                    const auto anchor_it = edit.find("anchor");
                    if (anchor_it == edit.end() || !anchor_it->is_string()) {
                        continue;
                    }

                    const auto anchor_text = anchor_it->get<std::string>();
                    const auto hash_pos = anchor_text.find('#');
                    if (hash_pos == std::string::npos) {
                        continue;
                    }

                    const auto line_text = utils::trim_copy(std::string_view(anchor_text).substr(0, hash_pos));
                    int line_number = 0;
                    const auto parse_result = std::from_chars(line_text.data(), line_text.data() + line_text.size(), line_number);
                    if (parse_result.ec == std::errc{} && parse_result.ptr == line_text.data() + line_text.size() && line_number > 0) {
                        append_path_field(paths, call.input, "path");
                        break;
                    }
                }
                if (!paths.empty()) {
                    return paths;
                }
            }
        }
        return paths;
    }

    inline void write_tool_stdout(std::string_view text) {
        if (!text.empty()) {
            static_cast<void>(std::fwrite(text.data(), sizeof(char), text.size(), stdout));
        }
        std::fflush(stdout);
    }

    [[nodiscard]]
    inline loop_status check_loop_detection(ToolCallCounts &call_counts, const ToolUse &call) {
        const auto input_hash = std::hash<std::string>{}(call.input.dump());
        ToolCallSignature signature{.name = call.name, .input_hash = input_hash};

        auto &count = call_counts[signature];
        ++count;

        if (count >= LOOP_ABORT_THRESHOLD) {
            spdlog::warn("Loop abort: tool '{}' called {} times with same input, forcing stop", call.name, count);
            return loop_status::abort;
        }
        if (count >= LOOP_DETECTION_THRESHOLD) {
            spdlog::warn("Loop detected: tool '{}' called {} times with same input", call.name, count);
            return loop_status::warning;
        }
        return loop_status::ok;
    }

    [[nodiscard]]
    inline std::pair<std::vector<Content>, loop_status> execute_tools(const std::vector<ToolUse> &calls, ToolRegistry &tools, ToolCallCounts &call_counts,
                                                                      HookManager *hook_manager, bool human_output, const AgentLoop::ToolEventCallback &on_tool_event,
                                                                      skills::SkillLoader *skill_loader) {
        struct ToolExecutionState {
            ToolUse call;
            loop_status status = loop_status::ok;
            std::optional<ToolResult> result;
        };

        struct ToolExecutionOutcome {
            ToolResult result;
            loop_status status = loop_status::ok;
        };

        std::vector<Content> result_blocks;
        loop_status worst_status = loop_status::ok;

        for (const auto &call : calls) {
            auto pipeline = stdexec::just(ToolExecutionState{.call = call}) | stdexec::then([&call_counts, human_output, &on_tool_event](ToolExecutionState state) {
                                if (const auto status = check_loop_detection(call_counts, state.call); status != loop_status::ok) {
                                    state.status = status;
                                    if (status == loop_status::abort) {
                                        return state;
                                    }
                                }
                                if (human_output) {
                                    std::string line;
                                    utils::format_to(line, "  -> ");
                                    utils::format_to(line, spdlog::fmt_lib::fg(spdlog::fmt_lib::terminal_color::cyan), "{}", state.call.name);
                                    utils::format_to(line, "\n");
                                    write_tool_stdout(line);
                                }
                                if (on_tool_event != nullptr) {
                                    on_tool_event("tool_started", state.call, nullptr);
                                }
                                return state;
                            }) |
                            stdexec::then([hook_manager](ToolExecutionState state) {
                                if (state.status == loop_status::abort) {
                                    state.result = ToolResult{state.call.id, "Tool call aborted because the agent repeated the same request too many times.", true};
                                    return state;
                                }
                                if (hook_manager == nullptr) {
                                    return state;
                                }

                                auto hook_ctx = build_before_tool_call_context(state.call.name, state.call.input);
                                const auto hook_result = hook_manager->dispatch(hook_event::before_tool_call, hook_ctx);
                                if (!hook_result.allowed) {
                                    std::string block_msg = "Tool call blocked by hook '" + hook_result.blocked_by + "'";
                                    if (!hook_result.block_reason.empty()) {
                                        block_msg += ": " + hook_result.block_reason;
                                    }
                                    state.result = ToolResult{state.call.id, std::move(block_msg), true};
                                }
                                return state;
                            }) |
                            stdexec::then([&tools, hook_manager, skill_loader](ToolExecutionState state) {
                                if (state.result.has_value()) {
                                    return state;
                                }

                                state.result = tools.execute(state.call);
                                if (skill_loader != nullptr && !state.result->is_error) {
                                    auto touched_paths = touched_paths_for_tool_call(state.call);
                                    if (!touched_paths.empty()) {
                                        skill_loader->activate_for_paths(touched_paths);
                                    }
                                }
                                if (hook_manager != nullptr) {
                                    auto hook_ctx = build_after_tool_call_context(state.call.name, state.call.input, state.result->content, state.result->is_error);
                                    static_cast<void>(hook_manager->dispatch(hook_event::after_tool_call, hook_ctx));
                                }
                                return state;
                            }) |
                            stdexec::then([&on_tool_event](ToolExecutionState state) {
                                if (on_tool_event != nullptr) {
                                    on_tool_event("tool_finished", state.call, &*state.result);
                                }
                                return ToolExecutionOutcome{
                                    .result = std::move(*state.result),
                                    .status = state.status,
                                };
                            });

            auto [outcome] = execution::sync_wait_or_throw(std::move(pipeline), "agent tool execution pipeline");
            worst_status = std::max(outcome.status, worst_status);
            result_blocks.emplace_back(std::move(outcome.result));
            if (outcome.status == loop_status::abort) {
                break;
            }
        }

        return {std::move(result_blocks), worst_status};
    }

} // namespace orangutan::agent::detail
