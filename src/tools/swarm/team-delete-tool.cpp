#include "tools/swarm/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "swarm/team-manager.hpp"
#include "swarm/mailbox.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace orangutan::tools {

    namespace {

        std::string team_delete_handler(const nlohmann::json &input, const ToolRuntimeContext &tool_context) {
            if (tool_context.team_manager == nullptr) {
                return nlohmann::json{{"deleted", false}, {"error", "Team manager is not available"}}.dump();
            }

            auto team_id = input.at("team_id").get<std::string>();

            spdlog::info("team_delete called: team_id={}", team_id);

            auto team = tool_context.team_manager->find_team(team_id);
            if (!team.has_value()) {
                return nlohmann::json{{"deleted", false}, {"error", "Team not found: " + team_id}}.dump();
            }

            tool_context.team_manager->abandon_active_members(team_id);
            tool_context.team_manager->delete_team(team_id);

            if (tool_context.mailbox != nullptr) {
                tool_context.mailbox->clear_team(team_id);
            }

            return nlohmann::json{{"deleted", true}, {"team_id", team_id}}.dump();
        }

    } // namespace

    void register_team_delete_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        registry.register_tool({.definition = {.name = "team_delete",
                                               .description = "Delete a team and deactivate all its members.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties", {{"team_id", {{"type", "string"}, {"description", "The ID of the team to delete"}}}}},
                                                                {"required", nlohmann::json::array({"team_id"})}}},
                                .execute =
                                    [tool_context](const nlohmann::json &input) {
                                        return team_delete_handler(input, *tool_context);
                                    },
                                .deferred = true});
    }

} // namespace orangutan::tools
