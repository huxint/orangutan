#pragma once

#include "permissions.hpp"
#include "types/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orangutan {

    class RuntimeMemory;
    struct ToolRuntimeContext;

    struct Tool {
        ToolDef definition;
        std::function<std::string(const nlohmann::json &input)> execute;
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

    void register_builtin_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory = nullptr, const std::string &workspace = {},
                                const ToolRuntimeContext *tool_context = nullptr, const ToolPermissionSettings *permissions = nullptr,
                                std::string_view edit_mode = "search_replace");

} // namespace orangutan
