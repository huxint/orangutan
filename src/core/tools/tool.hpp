#pragma once

#include "core/tools/permissions.hpp"
#include "core/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orangutan {

class CronStore;
class HeartbeatScheduler;
class RuntimeMemory;
class SubagentManager;

struct ToolRuntimeContext {
    std::string runtime_key;
    std::string agent_key;
    std::string scope_key;
    std::string *current_session_id = nullptr;
    std::vector<std::string> allowed_child_agents;
    bool is_child_run = false;
    SubagentManager *subagent_manager = nullptr;
    SubagentRuntimeOrigin runtime_origin = SubagentRuntimeOrigin::cli;
    std::string raw_caller_id;
    CronStore *cron_store = nullptr;
    HeartbeatScheduler *heartbeat_scheduler = nullptr;
    ToolApprovalCallback approval_callback;
};

struct Tool {
    ToolDef definition;
    std::function<std::string(const json &input)> execute;
};

class ToolRegistry {
public:
    using ExecutionGuard = std::function<std::optional<ToolResultBlock>(const ToolUseBlock &call)>;
    using DefinitionFilter = std::function<bool(const ToolDef &definition)>;

    void register_tool(Tool tool);
    void set_execution_guard(ExecutionGuard guard);
    void set_definition_filter(DefinitionFilter filter);

    std::vector<ToolDef> definitions() const;

    ToolResultBlock execute(const ToolUseBlock &call) const;

private:
    std::unordered_map<std::string, Tool> tools_;
    ExecutionGuard execution_guard_;
    DefinitionFilter definition_filter_;
};

void register_builtin_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory = nullptr, const std::string &workspace = {},
                            const ToolRuntimeContext *tool_context = nullptr, const ToolPermissionSettings *permissions = nullptr,
                            std::string_view edit_mode = "search_replace");

} // namespace orangutan
