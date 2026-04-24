#include "tools/orchestration/register.hpp"

#include <algorithm>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "orchestration/orchestration-manager.hpp"
#include "orchestration/types.hpp"
#include "orchestration/team-manager.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"

namespace orangutan::tools {

    namespace {

        [[nodiscard]]
        auto parse_agent_role(std::string_view role_str) -> std::optional<orangutan::orchestration::agent_role> {
            if (role_str == "worker") {
                return orangutan::orchestration::agent_role::worker;
            }
            if (role_str == "teammate") {
                return orangutan::orchestration::agent_role::teammate;
            }
            return std::nullopt;
        }

        auto agent_spawn_handler(const nlohmann::json &input, const ToolRuntimeContext &tool_context) -> std::string {
            auto agent_key = input.at("agent_key").get<std::string>();
            auto prompt = input.at("prompt").get<std::string>();
            auto name = input.value("name", std::string{});
            auto team = input.value("team", std::string{});
            auto role_str = input.value("role", std::string{"worker"});

            if (tool_context.orchestration_manager == nullptr) {
                return nlohmann::json{{"accepted", false}, {"error", "Orchestration manager not available"}}.dump();
            }

            if (!tool_context.team_agents.empty()) {
                const auto allowed = std::ranges::find(tool_context.team_agents, agent_key);
                if (allowed == tool_context.team_agents.end()) {
                    return nlohmann::json{
                        {"accepted", false},
                        {"error", "agent '" + agent_key + "' is not in the allowed team_agents list"},
                    }
                        .dump();
                }
            }

            std::string resolved_team_id;
            if (!team.empty()) {
                if (tool_context.team_manager == nullptr) {
                    return nlohmann::json{{"accepted", false}, {"error", "Team manager is not available"}}.dump();
                }

                auto team_record = tool_context.team_manager->find_team(team);
                if (!team_record.has_value()) {
                    team_record = tool_context.team_manager->find_team_by_name(team);
                }
                if (!team_record.has_value()) {
                    return nlohmann::json{{"accepted", false}, {"error", "Team not found: " + team}}.dump();
                }
                resolved_team_id = team_record->id;
            }

            const auto role = parse_agent_role(role_str);
            if (!role.has_value()) {
                return nlohmann::json{
                    {"accepted", false},
                    {"error", "Invalid role: " + role_str + ". Expected 'worker' or 'teammate'."},
                }
                    .dump();
            }

            auto result = tool_context.orchestration_manager->spawn(orangutan::orchestration::AgentSpawnRequest{
                .agent_key = agent_key,
                .agent_name = name,
                .task_prompt = prompt,
                .team_id = resolved_team_id,
                .parent_runtime_key = tool_context.runtime_key,
                .role = *role,
            });

            if (result.accepted && !resolved_team_id.empty() && tool_context.team_manager != nullptr) {
                tool_context.team_manager->add_member(orangutan::orchestration::TeamMemberRecord{
                    .agent_id = result.run_id,
                    .name = result.agent_name,
                    .agent_key = agent_key,
                    .team_id = resolved_team_id,
                });
            }

            return nlohmann::json{
                {"accepted", result.accepted},
                {"run_id", result.run_id},
                {"agent_name", result.agent_name},
                {"role", role_str},
                {"status", result.accepted ? "running" : "rejected"},
                {"error", result.error},
            }
                .dump();
        }

    } // namespace

    void register_agent_spawn_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (auto tool = make_tool_spec_builder("agent_spawn")
                            .description("Spawn a worker agent to handle a delegated task. "
                                         "Workers run a single task and report results. "
                                         "Teammates stay alive for follow-up messages after completing their initial task.")
                            .input_schema({{"type", "object"},
                                           {"properties",
                                            {{"agent_key", {{"type", "string"}, {"description", "The agent type to spawn (e.g. general-purpose, explorer, planner)"}}},
                                             {"prompt", {{"type", "string"}, {"description", "The task description and instructions for the agent"}}},
                                             {"name", {{"type", "string"}, {"description", "Optional human-readable name for this agent instance"}}},
                                             {"team", {{"type", "string"}, {"description", "Optional team ID to assign this agent to"}}},
                                             {"role",
                                              {{"type", "string"},
                                               {"description", "Agent lifecycle: 'worker' (fire-and-forget) or 'teammate' (persistent, waits for follow-up)"},
                                               {"enum", nlohmann::json::array({"worker", "teammate"})}}}}},
                                           {"required", nlohmann::json::array({"agent_key", "prompt"})}})
                            .execute([tool_context](const nlohmann::json &input) {
                                return agent_spawn_handler(input, *tool_context);
                            })
                            .build();
            tool.has_value()) {
            registry.register_tool(std::move(*tool));
        } else {
            spdlog::warn("failed to register tool: {}", tool.error());
        }
    }

} // namespace orangutan::tools
