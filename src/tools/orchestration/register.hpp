#pragma once

namespace orangutan::tools {
    struct OrchestrationCapability;
    struct RuntimeIdentityContext;
    struct ToolRuntimeContext;
    class ToolRegistry;

    void register_agent_spawn_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_agent_spawn_tool(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability);
    void register_agent_send_message_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_agent_send_message_tool(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability);
    void register_agent_stop_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_agent_stop_tool(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability);
    void register_team_create_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_team_create_tool(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability);
    void register_team_delete_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_team_delete_tool(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability);

    /// Register all orchestration tools (agent + team management).
    void register_orchestration_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_orchestration_tools(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability);

    /// Register only communication tools safe for delegated teammates.
    void register_agent_communication_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
    void register_agent_communication_tools(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability);
} // namespace orangutan::tools
