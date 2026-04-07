#include "tools/swarm/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"
#include "swarm/team-manager.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace orangutan::tools {

    namespace {

        std::string team_create_handler(const nlohmann::json &input, const ToolRuntimeContext &tool_context) {
            if (tool_context.team_manager == nullptr) {
                return nlohmann::json{{"created", false}, {"error", "Team manager is not available"}}.dump();
            }

            auto name = input.at("name").get<std::string>();
            auto description = input.value("description", std::string{});

            spdlog::info("team_create called: name={}", name);

            auto record = tool_context.team_manager->create_team(name, description, tool_context.agent_key);
            return nlohmann::json{
                {"created", true},
                {"team_id", record.id},
                {"name", record.name},
            }
                .dump();
        }

    } // namespace

    void register_team_create_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        registry.register_tool(tool_spec_builder("team_create")
                                   .description("Create a new team for organizing agents that collaborate on a shared task.")
                                   .input_schema({{"type", "object"},
                                                  {"properties",
                                                   {{"name", {{"type", "string"}, {"description", "A unique name for the team"}}},
                                                    {"description", {{"type", "string"}, {"description", "Optional description of the team's purpose"}}}}},
                                                  {"required", nlohmann::json::array({"name"})}})
                                   .execute([tool_context](const nlohmann::json &input) {
                                       return team_create_handler(input, *tool_context);
                                   })
                                   .deferred()
                                   .build());
    }

} // namespace orangutan::tools
