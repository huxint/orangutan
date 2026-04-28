#include "tools/orchestration/register.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "orchestration/mailbox.hpp"
#include "orchestration/orchestration-manager.hpp"
#include "orchestration/types.hpp"
#include "orchestration/team-manager.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"
#include "utils/enum-string.hpp"
#include "utils/format.hpp"

namespace orangutan::tools {

    namespace {

        [[nodiscard]]
        auto parse_relationship(std::string_view relationship) -> std::optional<orangutan::orchestration::teammate_relationship> {
            return orangutan::utils::parse_enum<orangutan::orchestration::teammate_relationship>(relationship);
        }

        struct AgentSpawnToolContext {
            std::string runtime_key;
            std::string agent_key;
            std::string agent_name;
            std::string team_id;
            orangutan::orchestration::OrchestrationManager *orchestration_manager = nullptr;
            orangutan::orchestration::TeamManager *team_manager = nullptr;
            orangutan::orchestration::AgentMailbox *mailbox = nullptr;
        };

        [[nodiscard]]
        auto make_agent_spawn_tool_context(RuntimeIdentityContext identity, OrchestrationCapability capability) -> AgentSpawnToolContext {
            return AgentSpawnToolContext{
                .runtime_key = std::string(identity.runtime_key),
                .agent_key = std::string(identity.agent_key),
                .agent_name = std::string(identity.agent_name),
                .team_id = std::string(identity.team_id),
                .orchestration_manager = capability.orchestration_manager,
                .team_manager = capability.team_manager,
                .mailbox = capability.mailbox,
            };
        }

        [[nodiscard]]
        auto sender_name(const AgentSpawnToolContext &tool_context) -> std::string {
            return tool_context.agent_name.empty() ? tool_context.agent_key : tool_context.agent_name;
        }

        void add_leader_member(const AgentSpawnToolContext &tool_context, const orangutan::orchestration::TeamRecord &team) {
            if (tool_context.team_manager == nullptr || tool_context.runtime_key.empty()) {
                return;
            }
            tool_context.team_manager->add_member(orangutan::orchestration::TeamMemberRecord{
                .agent_id = tool_context.runtime_key,
                .name = sender_name(tool_context),
                .config_agent_key = tool_context.agent_key,
                .team_id = team.id,
                .relationship = orangutan::orchestration::teammate_relationship::managed,
            });
        }

        [[nodiscard]]
        auto relationship_summary(orangutan::orchestration::teammate_relationship relationship) -> std::string_view {
            switch (relationship) {
            case orangutan::orchestration::teammate_relationship::managed:
                return "managed teammate: execute assigned work, report progress, and wait for leader direction";
            case orangutan::orchestration::teammate_relationship::peer:
                return "peer teammate: discuss, challenge assumptions, coordinate, and only change files when explicitly assigned implementation work";
            }
            return "teammate";
        }

        [[nodiscard]]
        auto team_title(const std::optional<orangutan::orchestration::TeamRecord> &team, std::string_view team_id) -> std::string {
            if (team.has_value()) {
                return fmt::format("{} ({})", team->name, team->id);
            }
            return std::string(team_id);
        }

        [[nodiscard]]
        auto with_preview_member(std::vector<orangutan::orchestration::TeamMemberRecord> members,
                                 std::string_view name,
                                 std::string_view config_agent_key,
                                 std::string_view team_id,
                                 orangutan::orchestration::teammate_relationship relationship) -> std::vector<orangutan::orchestration::TeamMemberRecord> {
            const auto exists = std::ranges::any_of(members, [name](const auto &member) {
                return member.name == name;
            });
            if (!exists) {
                members.push_back(orangutan::orchestration::TeamMemberRecord{
                    .agent_id = "pending",
                    .name = std::string(name),
                    .config_agent_key = std::string(config_agent_key),
                    .team_id = std::string(team_id),
                    .relationship = relationship,
                });
            }
            return members;
        }

        void append_team_context_body(std::string &out,
                                      const std::optional<orangutan::orchestration::TeamRecord> &team,
                                      std::string_view team_id,
                                      const std::vector<orangutan::orchestration::TeamMemberRecord> &members,
                                      std::string_view new_agent_name,
                                      std::string_view task,
                                      orangutan::orchestration::teammate_relationship relationship) {
            orangutan::utils::format_to(out, "Team: {}\n", team_title(team, team_id));
            orangutan::utils::format_to(out, "New teammate: `{}`\n", new_agent_name);
            orangutan::utils::format_to(out, "Relationship to leader: {} ({})\n", orangutan::utils::enum_name(relationship), relationship_summary(relationship));
            orangutan::utils::format_to(out, "Assignment: {}\n\n", task);

            out += "Communication:\n";
            out += "- Use `agent_send_message` with `to` set to a teammate name to contact that teammate.\n";
            out += "- Use `agent_send_message` with `to:\"*\"` to broadcast to the whole team.\n";
            out += "- Keep messages concise and include the context another teammate needs to act.\n\n";

            out += "Current roster:\n";
            if (members.empty()) {
                out += "- No active teammates have been recorded yet.\n";
                return;
            }
            for (const auto &member : members) {
                if (team.has_value() && member.agent_id == team->lead_agent_id) {
                    orangutan::utils::format_to(out, "- `{}` (leader, runtime `{}`)\n", member.name, member.agent_id);
                    continue;
                }
                orangutan::utils::format_to(out, "- `{}` ({}, runtime `{}`)\n", member.name, orangutan::utils::enum_name(member.relationship), member.agent_id);
            }
        }

        [[nodiscard]]
        auto render_initial_team_context(const std::optional<orangutan::orchestration::TeamRecord> &team,
                                         std::string_view team_id,
                                         const std::vector<orangutan::orchestration::TeamMemberRecord> &members,
                                         std::string_view new_agent_name,
                                         std::string_view task,
                                         orangutan::orchestration::teammate_relationship relationship) -> std::string {
            std::string out = "## Team Context\n";
            append_team_context_body(out, team, team_id, members, new_agent_name, task, relationship);
            return out;
        }

        [[nodiscard]]
        auto render_team_update(const std::optional<orangutan::orchestration::TeamRecord> &team,
                                std::string_view team_id,
                                const std::vector<orangutan::orchestration::TeamMemberRecord> &members,
                                std::string_view new_agent_name,
                                std::string_view task,
                                orangutan::orchestration::teammate_relationship relationship) -> std::string {
            std::string out;
            orangutan::utils::format_to(out, "Team update: `{}` has joined as a {} teammate.\n\n", new_agent_name, orangutan::utils::enum_name(relationship));
            append_team_context_body(out, team, team_id, members, new_agent_name, task, relationship);
            out += "\nNo reply is required unless this changes your current work.";
            return out;
        }

        [[nodiscard]]
        auto append_team_context(std::string instructions, std::string team_context) -> std::string {
            if (team_context.empty()) {
                return instructions;
            }
            if (!instructions.empty()) {
                instructions += "\n\n";
            }
            instructions += std::move(team_context);
            return instructions;
        }

        [[nodiscard]]
        auto find_team_record(const AgentSpawnToolContext &tool_context, const std::string &team_id) -> std::optional<orangutan::orchestration::TeamRecord> {
            if (tool_context.team_manager == nullptr || team_id.empty()) {
                return std::nullopt;
            }
            return tool_context.team_manager->find_team(team_id);
        }

        void broadcast_spawn_update(const AgentSpawnToolContext &tool_context,
                                    std::string_view team_id,
                                    const std::vector<std::string> &recipients,
                                    const orangutan::orchestration::AgentSpawnResult &result,
                                    std::string_view task,
                                    orangutan::orchestration::teammate_relationship relationship) {
            if (tool_context.mailbox == nullptr || tool_context.team_manager == nullptr || team_id.empty()) {
                return;
            }

            auto team_id_str = std::string(team_id);
            const auto team = tool_context.team_manager->find_team(team_id_str);
            const auto members = tool_context.team_manager->list_members(team_id_str);
            const auto update = render_team_update(team, team_id, members, result.agent_name, task, relationship);
            tool_context.mailbox->send_broadcast(team_id_str, sender_name(tool_context), update, recipients);
        }

        [[nodiscard]]
        auto resolve_team_id(const nlohmann::json &input, const AgentSpawnToolContext &tool_context) -> std::optional<std::string> {
            const auto team = input.value("team", std::string{});
            if (tool_context.team_manager == nullptr) {
                return std::nullopt;
            }

            if (!team.empty()) {
                auto team_record = tool_context.team_manager->find_team(team);
                if (!team_record.has_value()) {
                    team_record = tool_context.team_manager->find_team_by_name(team);
                }
                if (!team_record.has_value()) {
                    return std::nullopt;
                }
                add_leader_member(tool_context, *team_record);
                return team_record->id;
            }

            if (tool_context.team_id.empty()) {
                if (auto team_record = tool_context.team_manager->find_team_for_lead(tool_context.runtime_key); team_record.has_value()) {
                    return team_record->id;
                }
            } else {
                return tool_context.team_id;
            }

            const auto name = sender_name(tool_context) + " collaborators";
            auto team_record = tool_context.team_manager->create_team(name, "Dynamic teammate workspace", tool_context.runtime_key);
            add_leader_member(tool_context, team_record);
            return team_record.id;
        }

        auto agent_spawn_handler(const nlohmann::json &input, const AgentSpawnToolContext &tool_context) -> std::string {
            auto name = input.at("name").get<std::string>();
            auto task = input.at("task").get<std::string>();
            auto instructions = input.value("instructions", std::string{});
            auto relationship_str = input.value("relationship", std::string{"managed"});

            if (tool_context.orchestration_manager == nullptr) {
                return nlohmann::json{{"accepted", false}, {"error", "Orchestration manager not available"}}.dump();
            }

            const auto relationship = parse_relationship(relationship_str);
            if (!relationship.has_value()) {
                return nlohmann::json{
                    {"accepted", false},
                    {"error", "Invalid relationship: " + relationship_str + ". Expected 'managed' or 'peer'."},
                }
                    .dump();
            }

            auto resolved_team_id = resolve_team_id(input, tool_context).value_or(std::string{});
            if (!input.value("team", std::string{}).empty() && resolved_team_id.empty()) {
                return nlohmann::json{{"accepted", false}, {"error", "Team not found: " + input.value("team", std::string{})}}.dump();
            }

            if (!resolved_team_id.empty() && tool_context.team_manager != nullptr) {
                const auto team = find_team_record(tool_context, resolved_team_id);
                auto members = tool_context.team_manager->list_members(resolved_team_id);
                members = with_preview_member(std::move(members), name, tool_context.agent_key, resolved_team_id, *relationship);
                instructions = append_team_context(std::move(instructions),
                                                   render_initial_team_context(team, resolved_team_id, members, name, task, *relationship));
            }

            auto broadcast_recipients = std::vector<std::string>{};
            if (!resolved_team_id.empty() && tool_context.team_manager != nullptr) {
                broadcast_recipients = tool_context.team_manager->list_member_names(resolved_team_id);
            }

            auto result = tool_context.orchestration_manager->spawn(orangutan::orchestration::AgentSpawnRequest{
                .name = name,
                .instructions = instructions,
                .task = task,
                .team_id = resolved_team_id,
                .parent_runtime_key = tool_context.runtime_key,
                .config_agent_key = tool_context.agent_key,
                .profile_override = input.value("profile", std::string{}),
                .model_override = input.value("model", std::string{}),
                .thinking_budget_override = input.value("thinking_budget", 0),
                .relationship = *relationship,
            });

            if (result.accepted && !resolved_team_id.empty() && tool_context.team_manager != nullptr) {
                tool_context.team_manager->add_member(orangutan::orchestration::TeamMemberRecord{
                    .agent_id = result.run_id,
                    .name = result.agent_name,
                    .config_agent_key = tool_context.agent_key,
                    .team_id = resolved_team_id,
                    .relationship = *relationship,
                });
                broadcast_spawn_update(tool_context, resolved_team_id, broadcast_recipients, result, task, *relationship);
            }

            return nlohmann::json{
                {"accepted", result.accepted},
                {"run_id", result.run_id},
                {"name", result.agent_name},
                {"relationship", relationship_str},
                {"team_id", resolved_team_id},
                {"status", result.accepted ? "running" : "rejected"},
                {"error", result.error},
            }
                .dump();
        }

    } // namespace

    void register_agent_spawn_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }
        register_agent_spawn_tool(registry, runtime_identity_context(*tool_context), orchestration_capability(*tool_context));
    }

    void register_agent_spawn_tool(ToolRegistry &registry, RuntimeIdentityContext identity, OrchestrationCapability capability) {
        auto tool_context = make_agent_spawn_tool_context(identity, capability);
        if (auto tool = make_tool_spec_builder("agent_spawn")
                            .description("Create a named teammate. Teammates join a team, can communicate with each other, and inherit the current runtime unless profile/model overrides are provided.")
                            .input_schema({{"type", "object"},
                                           {"properties",
                                            {{"name", {{"type", "string"}, {"description", "Human-readable name for this agent instance"}}},
                                             {"task", {{"type", "string"}, {"description", "The initial task or message for the spawned agent"}}},
                                             {"instructions", {{"type", "string"}, {"description", "Optional behavior, persona, or operating instructions"}}},
                                             {"team", {{"type", "string"}, {"description", "Optional team ID or team name to assign this agent to"}}},
                                             {"relationship",
                                              {{"type", "string"},
                                               {"description", "Relationship to the leader: 'managed' for assigned execution, 'peer' for discussion/coordination"},
                                               {"enum", nlohmann::json::array({"managed", "peer"})}}},
                                             {"profile", {{"type", "string"}, {"description", "Optional config profile override for this spawned agent"}}},
                                             {"model", {{"type", "string"}, {"description", "Optional model override within the selected profile"}}},
                                              {"thinking_budget", {{"type", "integer"}, {"description", "Optional thinking budget override"}, {"minimum", 0}}}}},
                                           {"required", nlohmann::json::array({"name", "task"})}})
                            .execute([tool_context = std::move(tool_context)](const nlohmann::json &input) {
                                return agent_spawn_handler(input, tool_context);
                            })
                            .build();
            tool.has_value()) {
            registry.register_tool(std::move(*tool));
        } else {
            spdlog::warn("failed to register tool: {}", tool.error());
        }
    }

} // namespace orangutan::tools
