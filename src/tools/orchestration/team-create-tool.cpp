#include "tools/orchestration/register.hpp"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "orchestration/team-manager.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"

namespace orangutan::tools {

    namespace {

        struct TeamCreateToolContext {
            std::string runtime_key;
            std::string agent_key;
            std::string agent_name;
            orchestration::TeamManager *team_manager = nullptr;
        };

        [[nodiscard]]
        auto make_team_create_tool_context(RuntimeIdentityContext identity, OrchestrationCapability capability) -> TeamCreateToolContext {
            return TeamCreateToolContext{
                .runtime_key = std::string(identity.runtime_key),
                .agent_key = std::string(identity.agent_key),
                .agent_name = std::string(identity.agent_name),
                .team_manager = capability.team_manager,
            };
        }

        auto team_create_handler(const nlohmann::json &input, const TeamCreateToolContext &tool_context) -> std::string {
            if (tool_context.team_manager == nullptr) {
                return nlohmann::json{{"created", false}, {"error", "Team manager is not available"}}.dump();
            }

            auto name = input.at("name").get<std::string>();
            auto description = input.value("description", std::string{});

            spdlog::info("team_create called: name={}", name);

            auto record = tool_context.team_manager->create_team(name, description, tool_context.runtime_key);
            tool_context.team_manager->add_member(orchestration::TeamMemberRecord{
                .agent_id = tool_context.runtime_key,
                .name = tool_context.agent_name.empty() ? tool_context.agent_key : tool_context.agent_name,
                .config_agent_key = tool_context.agent_key,
                .team_id = record.id,
                .relationship = orchestration::teammate_relationship::managed,
            });
            return nlohmann::json{
                {"created", true},
                {"team_id", record.id},
                {"name", record.name},
            }
                .dump();
        }

    } // namespace

    void register_team_create_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }
        register_team_create_tool(registry, runtime_identity_context(*tool_context), orchestration_capability(*tool_context));
    }

    void register_team_create_tool(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability) {
        auto tool_context = make_team_create_tool_context(identity, capability);
        if (auto tool = make_tool_spec_builder("team_create")
                            .description("Create a team workspace for teammates collaborating on a shared task.")
                            .input_schema({{"type", "object"},
                                           {"properties",
                                            {{"name", {{"type", "string"}, {"description", "A unique name for the team"}}},
                                              {"description", {{"type", "string"}, {"description", "Optional description of the team's purpose"}}}}},
                                           {"required", nlohmann::json::array({"name"})}})
                            .execute([tool_context = std::move(tool_context)](const nlohmann::json &input) {
                                return team_create_handler(input, tool_context);
                            })
                            .build();
            tool.has_value()) {
            registry.register_tool(std::move(*tool));
        } else {
            spdlog::warn("failed to register tool: {}", tool.error());
        }
    }

} // namespace orangutan::tools
