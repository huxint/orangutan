#include "tools/coordinator/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "coordinator/coordinator-manager.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace orangutan::tools {

    namespace {

        std::string agent_spawn_handler(const nlohmann::json &input, const ToolRuntimeContext &tool_context) {
            auto agent_key = input.at("agent_key").get<std::string>();
            auto prompt = input.at("prompt").get<std::string>();
            auto name = input.value("name", std::string{});
            auto team = input.value("team", std::string{});

            if (tool_context.coordinator_manager == nullptr) {
                return nlohmann::json{{"accepted", false}, {"error", "Coordinator manager not available"}}.dump();
            }

            auto result = tool_context.coordinator_manager->spawn(orangutan::coordinator::AgentSpawnRequest{
                .agent_key = agent_key,
                .agent_name = name,
                .task_prompt = prompt,
                .team_id = team,
                .parent_runtime_key = tool_context.runtime_key,
            });

            return nlohmann::json{
                {"accepted", result.accepted},
                {"run_id", result.run_id},
                {"agent_name", result.agent_name},
                {"error", result.error},
            }
                .dump();
        }

    } // namespace

    void register_agent_spawn_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        registry.register_tool(
            {.definition = {.name = "agent_spawn",
                            .description = "Spawn a worker agent to handle a delegated task. The agent will run asynchronously and report results when complete.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"agent_key", {{"type", "string"}, {"description", "The agent type to spawn (e.g. general-purpose, explorer, planner)"}}},
                                               {"prompt", {{"type", "string"}, {"description", "The task description and instructions for the agent"}}},
                                               {"name", {{"type", "string"}, {"description", "Optional human-readable name for this agent instance"}}},
                                               {"team", {{"type", "string"}, {"description", "Optional team ID to assign this agent to"}}}}},
                                             {"required", nlohmann::json::array({"agent_key", "prompt"})}}},
             .execute =
                 [tool_context](const nlohmann::json &input) {
                     return agent_spawn_handler(input, *tool_context);
                 },
             .deferred = true});
    }

} // namespace orangutan::tools
