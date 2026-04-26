#include "tools/orchestration/register.hpp"

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "orchestration/orchestration-manager.hpp"
#include "orchestration/mailbox.hpp"
#include "orchestration/team-manager.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"

namespace orangutan::tools {

    namespace {

        auto agent_send_message_handler(const nlohmann::json &input, const ToolRuntimeContext &tool_context) -> std::string {
            auto run_id = input.value("run_id", std::string{});
            auto to = input.value("to", std::string{});
            auto text = input.at("text").get<std::string>();

            if (tool_context.orchestration_manager == nullptr) {
                return nlohmann::json{{"sent", false}, {"error", "Orchestration manager not available"}}.dump();
            }

            if (run_id.empty() && to.empty()) {
                return nlohmann::json{{"sent", false}, {"error", "Either run_id or to must be specified"}}.dump();
            }

            const auto sender_name = tool_context.agent_name.empty() ? tool_context.agent_key : tool_context.agent_name;

            // Send by run_id (directed message via mailbox)
            if (!run_id.empty()) {
                if (const auto error = tool_context.orchestration_manager->send_message(run_id, sender_name, text); error.has_value()) {
                    return nlohmann::json{{"sent", false}, {"error", *error}}.dump();
                }
                return nlohmann::json{{"sent", true}}.dump();
            }

            // Send by agent name within a team
            auto team_id = tool_context.team_id;
            if (team_id.empty() && tool_context.team_manager != nullptr) {
                if (auto team = tool_context.team_manager->find_team_for_lead(tool_context.runtime_key); team.has_value()) {
                    team_id = team->id;
                }
            }
            if (team_id.empty()) {
                return nlohmann::json{{"sent", false}, {"error", "Team context is required when addressing by name"}}.dump();
            }

            // Broadcast to all team members
            if (to == "*") {
                if (const auto error = tool_context.orchestration_manager->broadcast_message(team_id, sender_name, text); error.has_value()) {
                    return nlohmann::json{{"sent", false}, {"error", *error}}.dump();
                }
                return nlohmann::json{{"sent", true}}.dump();
            }

            // Directed message to a specific teammate
            if (const auto error = tool_context.orchestration_manager->send_message_by_name(team_id, sender_name, to, text); error.has_value()) {
                return nlohmann::json{{"sent", false}, {"error", *error}}.dump();
            }
            return nlohmann::json{{"sent", true}}.dump();
        }

    } // namespace

    void register_agent_send_message_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (auto tool = make_tool_spec_builder("agent_send_message")
                            .description("Send a message to a running agent. Can address by run_id, by agent name within a team, or broadcast to all team members with to='*'.")
                            .input_schema({{"type", "object"},
                                           {"properties",
                                            {{"run_id", {{"type", "string"}, {"description", "The run ID of the target agent"}}},
                                             {"to", {{"type", "string"}, {"description", "The agent name to send to (alternative to run_id). Use '*' for broadcast."}}},
                                             {"text", {{"type", "string"}, {"description", "The message text to send"}}}}},
                                           {"required", nlohmann::json::array({"text"})}})
                            .execute([tool_context](const nlohmann::json &input) {
                                return agent_send_message_handler(input, *tool_context);
                            })
                            .build();
            tool.has_value()) {
            registry.register_tool(std::move(*tool));
        } else {
            spdlog::warn("failed to register tool: {}", tool.error());
        }
    }

} // namespace orangutan::tools
