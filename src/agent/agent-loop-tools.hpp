#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "agent/agent-loop.hpp"
#include "hooks/hook-manager.hpp"
#include "skills/skill-loader.hpp"
#include "utils/format.hpp"
#include "utils/json-dump.hpp"
#include "utils/string.hpp"

namespace orangutan::agent::detail {

    inline constexpr int LOOP_DETECTION_THRESHOLD = 3;
    inline constexpr int LOOP_ABORT_THRESHOLD = 5;

    enum class loop_status : std::uint8_t {
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
        const auto input_hash = std::hash<std::string>{}(utils::json_dump_lossy(call.input));
        ToolCallSignature signature{.name = call.name, .input_hash = input_hash};

        auto &count = call_counts[signature];
        ++count;

        if (count >= LOOP_ABORT_THRESHOLD) {
            spdlog::warn("loop abort: tool '{}' called {} times with same input, forcing stop", call.name, count);
            return loop_status::abort;
        }
        if (count >= LOOP_DETECTION_THRESHOLD) {
            spdlog::warn("loop detected: tool '{}' called {} times with same input", call.name, count);
            return loop_status::warning;
        }
        return loop_status::ok;
    }

    struct ToolExecutionOutcome {
        ToolResult result;
        loop_status status = loop_status::ok;
    };

    inline void emit_tool_started(const ToolUse &call, bool human_output, const AgentLoop::ToolEventCallback &on_tool_event) {
        if (human_output) {
            std::string line;
            utils::format_to(line, "  -> ");
            utils::format_to(line, fmt::fg(fmt::terminal_color::cyan), "{}", call.name);
            utils::format_to(line, "\n");
            write_tool_stdout(line);
        }
        if (on_tool_event != nullptr) {
            on_tool_event("tool_started", call, nullptr);
        }
    }

    [[nodiscard]]
    inline std::optional<ToolResult> run_before_tool_hook(const ToolUse &call, HookManager *hook_manager) {
        if (hook_manager == nullptr) {
            return std::nullopt;
        }

        auto hook_ctx = build_before_tool_call_context(call.name, call.input);
        const auto hook_result = hook_manager->dispatch(hook_event::before_tool_call, hook_ctx);
        if (hook_result.allowed) {
            return std::nullopt;
        }

        std::string block_msg = "Tool call blocked by hook '" + hook_result.blocked_by + "'";
        if (!hook_result.block_reason.empty()) {
            block_msg += ": " + hook_result.block_reason;
        }
        return ToolResult{call.id, std::move(block_msg), true};
    }

    inline void activate_skills_for_tool_result(const ToolUse &call, const ToolResult &result, skills::SkillLoader *skill_loader) {
        if (skill_loader == nullptr || result.is_error) {
            return;
        }

        auto touched_paths = touched_paths_for_tool_call(call);
        if (!touched_paths.empty()) {
            skill_loader->activate_for_paths(touched_paths);
        }
    }

    inline void run_after_tool_hook(const ToolUse &call, const ToolResult &result, HookManager *hook_manager) {
        if (hook_manager == nullptr) {
            return;
        }

        auto hook_ctx = build_after_tool_call_context(call.name, call.input, result.content, result.is_error);
        static_cast<void>(hook_manager->dispatch(hook_event::after_tool_call, hook_ctx));
    }

    [[nodiscard]]
    inline ToolExecutionOutcome execute_single_tool_call(const ToolUse &call, ToolRegistry &tools, ToolCallCounts &call_counts, HookManager *hook_manager, bool human_output,
                                                        const AgentLoop::ToolEventCallback &on_tool_event, skills::SkillLoader *skill_loader) {
        const auto status = check_loop_detection(call_counts, call);
        if (status == loop_status::abort) {
            ToolResult result{call.id, "Tool call aborted because the agent repeated the same request too many times.", true};
            if (on_tool_event != nullptr) {
                on_tool_event("tool_finished", call, &result);
            }
            return ToolExecutionOutcome{
                .result = std::move(result),
                .status = status,
            };
        }

        emit_tool_started(call, human_output, on_tool_event);

        auto blocked_result = run_before_tool_hook(call, hook_manager);
        auto result = blocked_result.has_value() ? std::move(*blocked_result) : tools.execute(call);
        if (!blocked_result.has_value()) {
            activate_skills_for_tool_result(call, result, skill_loader);
            run_after_tool_hook(call, result, hook_manager);
        }

        if (on_tool_event != nullptr) {
            on_tool_event("tool_finished", call, &result);
        }

        return ToolExecutionOutcome{
            .result = std::move(result),
            .status = status,
        };
    }

    [[nodiscard]]
    inline std::pair<std::vector<Content>, loop_status> execute_tools(const std::vector<ToolUse> &calls, ToolRegistry &tools, ToolCallCounts &call_counts,
                                                                      HookManager *hook_manager, bool human_output, const AgentLoop::ToolEventCallback &on_tool_event,
                                                                      skills::SkillLoader *skill_loader) {
        std::vector<Content> result_blocks;
        loop_status worst_status = loop_status::ok;

        for (const auto &call : calls) {
            auto outcome = execute_single_tool_call(call, tools, call_counts, hook_manager, human_output, on_tool_event, skill_loader);
            worst_status = std::max(outcome.status, worst_status);
            result_blocks.emplace_back(std::move(outcome.result));
            if (outcome.status == loop_status::abort) {
                break;
            }
        }

        return {std::move(result_blocks), worst_status};
    }

} // namespace orangutan::agent::detail
