#include "agent/tool-executor.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/color.h>
#include <spdlog/spdlog.h>

#include "hooks/hook-manager.hpp"
#include "skills/skill-loader.hpp"
#include "utils/format.hpp"
#include "utils/json-dump.hpp"

namespace orangutan::agent {

    namespace {

        constexpr int LOOP_DETECTION_THRESHOLD = 3;
        constexpr int LOOP_ABORT_THRESHOLD = 5;

        void write_stdout(std::string_view text) {
            if (!text.empty()) {
                static_cast<void>(std::fwrite(text.data(), sizeof(char), text.size(), stdout));
            }
            std::fflush(stdout);
        }

        void append_path_field(std::vector<std::filesystem::path> &paths, const nlohmann::json &input, std::string_view key) {
            const auto it = input.find(std::string(key));
            if (it == input.end()) {
                return;
            }
            if (it->is_string()) {
                paths.emplace_back(it->get<std::string>());
            }
        }

        void append_paths_field(std::vector<std::filesystem::path> &paths, const nlohmann::json &input, std::string_view key) {
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
        std::vector<std::filesystem::path> touched_paths_for_tool_call(const ToolUse &call) {
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

        void emit_tool_started(const ToolUse &call, bool human_output, const ToolEventCallback &on_tool_event) {
            if (human_output) {
                std::string line;
                utils::format_to(line, "  -> ");
                utils::format_to(line, fmt::fg(fmt::terminal_color::cyan), "{}", call.name);
                utils::format_to(line, "\n");
                write_stdout(line);
            }
            if (on_tool_event != nullptr) {
                on_tool_event("tool_started", call, nullptr);
            }
        }

        [[nodiscard]]
        std::optional<ToolResult> run_before_tool_hook(const ToolUse &call, hooks::HookManager *hook_manager) {
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

        void activate_skills_for_tool_result(const ToolUse &call, const ToolResult &result, skills::SkillLoader *skill_loader) {
            if (skill_loader == nullptr || result.is_error) {
                return;
            }

            auto touched_paths = touched_paths_for_tool_call(call);
            if (!touched_paths.empty()) {
                skill_loader->activate_for_paths(touched_paths);
            }
        }

        void run_after_tool_hook(const ToolUse &call, const ToolResult &result, hooks::HookManager *hook_manager) {
            if (hook_manager == nullptr) {
                return;
            }

            auto hook_ctx = build_after_tool_call_context(call.name, call.input, result.content, result.is_error);
            static_cast<void>(hook_manager->dispatch(hook_event::after_tool_call, hook_ctx));
        }

    } // namespace

    std::size_t ToolExecutionState::SignatureHash::operator()(const ToolCallSignature &sig) const {
        const auto name_hash = std::hash<std::string>{}(sig.name);
        return name_hash ^ (sig.input_hash << 1);
    }

    tool_loop_status ToolExecutionState::check_loop_detection(const ToolUse &call) {
        const auto input_hash = std::hash<std::string>{}(utils::json_dump_lossy(call.input));
        ToolCallSignature signature{.name = call.name, .input_hash = input_hash};

        auto &count = call_counts_[signature];
        ++count;

        if (count >= LOOP_ABORT_THRESHOLD) {
            spdlog::warn("loop abort: tool '{}' called {} times with same input, forcing stop", call.name, count);
            return tool_loop_status::abort;
        }
        if (count >= LOOP_DETECTION_THRESHOLD) {
            spdlog::warn("loop detected: tool '{}' called {} times with same input", call.name, count);
            return tool_loop_status::warning;
        }
        return tool_loop_status::ok;
    }

    ToolExecutionState::SingleToolExecutionResult ToolExecutionState::execute_single_tool_call(const ToolUse &call, ToolRegistry &tools, hooks::HookManager *hook_manager,
                                                                                               bool human_output, const ToolEventCallback &on_tool_event,
                                                                                               skills::SkillLoader *skill_loader) {
        const auto status = check_loop_detection(call);
        if (status == tool_loop_status::abort) {
            ToolResult result{call.id, "Tool call aborted because the agent repeated the same request too many times.", true};
            if (on_tool_event != nullptr) {
                on_tool_event("tool_finished", call, &result);
            }
            return SingleToolExecutionResult{
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

        return SingleToolExecutionResult{
            .result = std::move(result),
            .status = status,
        };
    }

    ToolExecutionResult ToolExecutionState::execute(std::span<const ToolUse> calls, ToolRegistry &tools, hooks::HookManager *hook_manager, bool human_output,
                                                    const ToolEventCallback &on_tool_event, skills::SkillLoader *skill_loader) {
        ToolExecutionResult result;

        for (const auto &call : calls) {
            auto outcome = execute_single_tool_call(call, tools, hook_manager, human_output, on_tool_event, skill_loader);
            result.status = std::max(outcome.status, result.status);
            result.result_blocks.emplace_back(std::move(outcome.result));
            if (outcome.status == tool_loop_status::abort) {
                break;
            }
        }

        return result;
    }

} // namespace orangutan::agent
