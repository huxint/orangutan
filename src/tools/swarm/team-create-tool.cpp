#include "tools/swarm/register.hpp"
#include "tools/registry/tool-context.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace orangutan::tools {

    namespace {

        std::string team_create_handler(const nlohmann::json &input, const ToolRuntimeContext & /*tool_context*/) {
            auto name = input.at("name").get<std::string>();
            auto description = input.value("description", std::string{});

            spdlog::info("team_create called: name={}", name);

            // Stub: team_manager integration will be added when wiring is complete
            return nlohmann::json{
                {"created", false},
                {"team_id", ""},
                {"error", "Team manager not yet wired up"},
            }
                .dump();
        }

    } // namespace

    void register_team_create_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        registry.register_tool({.definition = {.name = "team_create",
                                               .description = "Create a new team for organizing agents that collaborate on a shared task.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"name", {{"type", "string"}, {"description", "A unique name for the team"}}},
                                                                  {"description", {{"type", "string"}, {"description", "Optional description of the team's purpose"}}}}},
                                                                {"required", nlohmann::json::array({"name"})}}},
                                .execute =
                                    [tool_context](const nlohmann::json &input) {
                                        return team_create_handler(input, *tool_context);
                                    },
                                .deferred = true});
    }

} // namespace orangutan::tools
