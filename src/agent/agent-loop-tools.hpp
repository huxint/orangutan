#pragma once

#include <algorithm>
#include <cstdio>
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
#include "utils/format.hpp"
#include "utils/sender-utils.hpp"

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
                                                                      HookManager *hook_manager, bool human_output, const AgentLoop::ToolEventCallback &on_tool_event) {
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
                            stdexec::then([&tools, hook_manager](ToolExecutionState state) {
                                if (state.result.has_value()) {
                                    return state;
                                }

                                state.result = tools.execute(state.call);
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
