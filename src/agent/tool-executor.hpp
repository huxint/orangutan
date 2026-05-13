#pragma once

#include "tools/registry/tool-registry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::skills {
    class SkillLoader;
}

namespace orangutan::agent {

    using ToolEventCallback = std::function<void(const std::string &event_type, const ToolUse &call, const ToolResult *result)>;

    enum class tool_loop_status : std::uint8_t {
        ok,
        warning,
        abort,
    };

    struct ToolExecutionResult {
        std::vector<Content> result_blocks;
        tool_loop_status status = tool_loop_status::ok;
    };

    class ToolExecutionState {
    public:
        [[nodiscard]]
        ToolExecutionResult execute(std::span<const ToolUse> calls, ToolRegistry &tools, hooks::HookManager *hook_manager, bool human_output,
                                    const ToolEventCallback &on_tool_event, skills::SkillLoader *skill_loader);

    private:
        struct ToolCallSignature {
            std::string name;
            std::size_t input_hash = 0;

            bool operator==(const ToolCallSignature &other) const = default;
        };

        struct SignatureHash {
            std::size_t operator()(const ToolCallSignature &sig) const;
        };

        struct SingleToolExecutionResult {
            ToolResult result;
            tool_loop_status status = tool_loop_status::ok;
        };

        [[nodiscard]]
        tool_loop_status check_loop_detection(const ToolUse &call);

        [[nodiscard]]
        SingleToolExecutionResult execute_single_tool_call(const ToolUse &call, ToolRegistry &tools, hooks::HookManager *hook_manager, bool human_output,
                                                           const ToolEventCallback &on_tool_event, skills::SkillLoader *skill_loader);

        std::unordered_map<ToolCallSignature, int, SignatureHash> call_counts_;
    };

} // namespace orangutan::agent
