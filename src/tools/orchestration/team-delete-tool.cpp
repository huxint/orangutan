#include "tools/orchestration/register.hpp"

#include <chrono>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "orchestration/mailbox.hpp"
#include "orchestration/orchestration-manager.hpp"
#include "orchestration/team-manager.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"

namespace orangutan::tools {

    namespace {

        constexpr int K_DEFAULT_GRACE_PERIOD_MS = 5000;

        struct TeamDeleteToolContext {
            std::string agent_key;
            std::string agent_name;
            orchestration::OrchestrationManager *orchestration_manager = nullptr;
            orchestration::TeamManager *team_manager = nullptr;
            orchestration::AgentMailbox *mailbox = nullptr;
        };

        [[nodiscard]]
        auto make_team_delete_tool_context(RuntimeIdentityContext identity, OrchestrationCapability capability) -> TeamDeleteToolContext {
            return TeamDeleteToolContext{
                .agent_key = std::string(identity.agent_key),
                .agent_name = std::string(identity.agent_name),
                .orchestration_manager = capability.orchestration_manager,
                .team_manager = capability.team_manager,
                .mailbox = capability.mailbox,
            };
        }

        auto team_delete_handler(const nlohmann::json &input, const TeamDeleteToolContext &tool_context) -> std::string {
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

            std::size_t stopped_runs = 0;
            if (tool_context.orchestration_manager != nullptr) {
                stopped_runs = tool_context.orchestration_manager->stop_team(team_id, std::chrono::milliseconds{grace_period_ms});
            }

            tool_context.team_manager->abandon_active_members(team_id);
            tool_context.team_manager->delete_team(team_id);

            return nlohmann::json{{"deleted", true},
                                  {"team_id", team_id},
                                  {"grace_period_ms", grace_period_ms},
                                  {"shutdown_requests_sent", active_members.size()},
                                  {"stopped_runs", stopped_runs}}
                .dump();
        }

    } // namespace

    void register_team_delete_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }
        register_team_delete_tool(registry, runtime_identity_context(*tool_context), orchestration_capability(*tool_context));
    }

    void register_team_delete_tool(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability) {
        auto tool_context = make_team_delete_tool_context(identity, capability);
        if (auto tool = make_tool_spec_builder("team_delete")
                            .description("Delete a team and deactivate all its members.")
                            .input_schema(
                                {{"type", "object"},
                                 {"properties",
                                  {{"team_id", {{"type", "string"}, {"description", "The ID of the team to delete"}}},
                                    {"grace_period_ms", {{"type", "integer"}, {"description", "Optional grace period in milliseconds before deleting the team"}, {"minimum", 0}}}}},
                                 {"required", nlohmann::json::array({"team_id"})}})
                            .execute([tool_context = std::move(tool_context)](const nlohmann::json &input) {
                                return team_delete_handler(input, tool_context);
                            })
                            .build();
            tool.has_value()) {
            registry.register_tool(std::move(*tool));
        } else {
            spdlog::warn("failed to register tool: {}", tool.error());
        }
    }

} // namespace orangutan::tools
