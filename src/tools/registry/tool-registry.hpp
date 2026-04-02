#pragma once

#include "permissions.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orangutan::memory {
    class RuntimeMemory;
}

namespace orangutan::tools {

    struct ToolRuntimeContext;

    struct ToolOutput {
        std::string text;
        std::vector<ToolResult::ImageBlock> images;

        ToolOutput() = default;
        ToolOutput(std::string t)
        : text(std::move(t)) {} // NOLINT(google-explicit-constructor)
        ToolOutput(std::string t, std::vector<ToolResult::ImageBlock> imgs)
        : text(std::move(t)),
          images(std::move(imgs)) {}
    };

    struct Tool {
        ToolDef definition;
        std::function<std::string(const nlohmann::json &input)> execute;
        std::function<ToolOutput(const nlohmann::json &input)> execute_rich;
    };

    class ToolRegistry {
    public:
        using ExecutionGuard = std::function<std::optional<ToolResult>(const ToolUse &call)>;
        using DefinitionFilter = std::function<bool(const ToolDef &definition)>;

        void register_tool(Tool tool);
        void set_execution_guard(ExecutionGuard guard);
        void set_definition_filter(DefinitionFilter filter);

        std::vector<ToolDef> definitions() const;

        ToolResult execute(const ToolUse &call) const;

    private:
        std::unordered_map<std::string, Tool> tools_;
        ExecutionGuard execution_guard_;
        DefinitionFilter definition_filter_;
    };

    [[nodiscard]]
    std::string scrub_tool_output(std::string_view text);

    void register_builtin_tools(ToolRegistry &registry, memory::RuntimeMemory *runtime_memory = nullptr, const std::string &workspace = {},
                                const ToolRuntimeContext *tool_context = nullptr, const ToolPermissionSettings *permissions = nullptr,
                                std::string_view edit_mode = "search_replace");

} // namespace orangutan::tools

namespace orangutan {

    using tools::register_builtin_tools;
    using tools::scrub_tool_output;
    using tools::Tool;
    using tools::ToolRegistry;

} // namespace orangutan
