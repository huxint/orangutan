#include "tools/orchestration/register.hpp"

#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "orchestration/mailbox.hpp"
#include "orchestration/team-manager.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"

namespace orangutan::tools {

    namespace {

        constexpr int K_DEFAULT_GRACE_PERIOD_MS = 5000;

        auto team_delete_handler(const nlohmann::json &input, const ToolRuntimeContext &tool_context) -> std::string {
            if (tool_context.team_manager == nullptr) {
                return nlohmann::json{{"deleted", false}, {"error", "Team manager is not available"}}.dump();
            }

            auto team_id = input.at("team_id").get<std::string>();
            auto grace_period_ms = input.value("grace_period_ms", K_DEFAULT_GRACE_PERIOD_MS);
            if (grace_period_ms < 0) {
                return nlohmann::json{{"deleted", false}, {"error", "grace_period_ms must be >= 0"}}.dump();
            }

            spdlog::info("team_delete called: team_id={}, grace_period_ms={}", team_id, grace_period_ms);

            auto team = tool_context.team_manager->find_team(team_id);
            if (!team.has_value()) {
                return nlohmann::json{{"deleted", false}, {"error", "team not found: " + team_id}}.dump();
            }

            auto active_members = tool_context.team_manager->list_members(team_id);
            auto sender = tool_context.agent_name;
            if (sender.empty()) {
                sender = tool_context.agent_key.empty() ? std::string{"system"} : tool_context.agent_key;
            }

            if (tool_context.mailbox != nullptr) {
                for (const auto &member : active_members) {
                    tool_context.mailbox->send(team_id, sender, member.name, "Team shutdown requested", orangutan::orchestration::message_type::shutdown_request);
                }
            }

            if (grace_period_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(grace_period_ms));
            }

            tool_context.team_manager->abandon_active_members(team_id);
            tool_context.team_manager->delete_team(team_id);

            return nlohmann::json{{"deleted", true}, {"team_id", team_id}, {"grace_period_ms", grace_period_ms}, {"shutdown_requests_sent", active_members.size()}}.dump();
        }

    } // namespace

    void register_team_delete_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (auto tool = make_tool_spec_builder("team_delete")
                            .description("Delete a team and deactivate all its members.")
                            .input_schema({{"type", "object"},
                                           {"properties",
                                            {{"team_id", {{"type", "string"}, {"description", "The ID of the team to delete"}}},
                                             {"grace_period_ms", {{"type", "integer"}, {"description", "Optional grace period in milliseconds before deleting the team"}, {"minimum", 0}}}}},
                                           {"required", nlohmann::json::array({"team_id"})}})
                            .execute([tool_context](const nlohmann::json &input) {
                                return team_delete_handler(input, *tool_context);
                            })
                            .deferred()
                            .build();
            tool.has_value()) {
            registry.register_tool(std::move(*tool));
        } else {
            spdlog::warn("failed to register tool: {}", tool.error());
        }
    }

} // namespace orangutan::tools
