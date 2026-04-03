#include "tools/swarm/register.hpp"
#include "tools/registry/tool-context.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace orangutan::tools {

    namespace {

        std::string team_delete_handler(const nlohmann::json &input, const ToolRuntimeContext & /*tool_context*/) {
            auto team_id = input.at("team_id").get<std::string>();

            spdlog::info("team_delete called: team_id={}", team_id);

            // Stub: team_manager integration will be added when wiring is complete
            return nlohmann::json{
                {"deleted", false},
                {"error", "Team manager not yet wired up"},
            }
                .dump();
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
