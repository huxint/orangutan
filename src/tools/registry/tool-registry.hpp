#pragma once

#include "permissions.hpp"
#include "permissions/permission-types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
        bool deferred = false;
    };

    struct DeferredToolSummary {
        std::string_view name;
        std::string_view description;
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

        // Deferred tool discovery
        void discover_tool(const std::string &name) const;
        void clear_discovered() const;
        [[nodiscard]]
        bool has_deferred_tools() const;
        [[nodiscard]]
        std::vector<DeferredToolSummary> deferred_tool_summaries() const;
        [[nodiscard]]
        const ToolDef *find_definition(const std::string &name) const;

    private:
        std::unordered_map<std::string, Tool> tools_;
        ExecutionGuard execution_guard_;
        DefinitionFilter definition_filter_;
        mutable std::unordered_set<std::string> discovered_tools_;
    };

    [[nodiscard]]
    std::string scrub_tool_output(std::string_view text);

    void register_builtin_tools(ToolRegistry &registry, memory::RuntimeMemory *runtime_memory = nullptr, const std::string &workspace = {},
                                const ToolRuntimeContext *tool_context = nullptr, const ToolPermissionContext *permissions = nullptr,
                                std::string_view edit_mode = "search_replace");

} // namespace orangutan::tools

namespace orangutan {

    using tools::register_builtin_tools;
    using tools::scrub_tool_output;
    using tools::Tool;
    using tools::ToolRegistry;

} // namespace orangutan
