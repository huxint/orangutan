#include "tools/coordinator/register.hpp"

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "coordinator/coordinator-manager.hpp"
#include "swarm/mailbox.hpp"
#include "swarm/team-manager.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"

namespace orangutan::tools {

    namespace {

        std::string agent_send_message_handler(const nlohmann::json &input, const ToolRuntimeContext &tool_context) {
            auto run_id = input.value("run_id", std::string{});
            auto to = input.value("to", std::string{});
            auto text = input.at("text").get<std::string>();

            if (tool_context.coordinator_manager == nullptr) {
                return nlohmann::json{{"sent", false}, {"error", "Coordinator manager not available"}}.dump();
            }

            if (run_id.empty() && to.empty()) {
                return nlohmann::json{{"sent", false}, {"error", "Either run_id or to must be specified"}}.dump();
            }

            if (!run_id.empty()) {
                if (const auto error =
                        tool_context.coordinator_manager->send_message(run_id, tool_context.agent_name.empty() ? tool_context.agent_key : tool_context.agent_name, text);
                    error.has_value()) {
                    return nlohmann::json{{"sent", false}, {"error", *error}}.dump();
                }
                return nlohmann::json{{"sent", true}}.dump();
            }

            if (tool_context.mailbox == nullptr || tool_context.team_manager == nullptr || tool_context.team_id.empty()) {
                return nlohmann::json{{"sent", false}, {"error", "Team mailbox is not available in this runtime"}}.dump();
            }

            auto member_names = tool_context.team_manager->list_member_names(tool_context.team_id);
            const auto sender_name = tool_context.agent_name.empty() ? tool_context.agent_key : tool_context.agent_name;

            if (to == "*") {
                tool_context.mailbox->send_broadcast(tool_context.team_id, sender_name, text, member_names);
                return nlohmann::json{{"sent", true}}.dump();
            }

            const auto found = std::ranges::find(member_names, to);
            if (found == member_names.end()) {
                return nlohmann::json{{"sent", false}, {"error", "agent '" + to + "' not found in team"}}.dump();
            }

            tool_context.mailbox->send(tool_context.team_id, sender_name, to, text);

            return nlohmann::json{{"sent", true}}.dump();
        }

    } // namespace

    void register_agent_send_message_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (auto tool = make_tool_spec_builder("agent_send_message")
                            .description("Send a message to a running agent. Can address by run_id or agent name.")
                            .input_schema({{"type", "object"},
                                           {"properties",
                                            {{"run_id", {{"type", "string"}, {"description", "The run ID of the target agent"}}},
                                             {"to", {{"type", "string"}, {"description", "The agent name to send to (alternative to run_id)"}}},
                                             {"text", {{"type", "string"}, {"description", "The message text to send"}}}}},
                                           {"required", nlohmann::json::array({"text"})}})
                            .execute([tool_context](const nlohmann::json &input) {
                                return agent_send_message_handler(input, *tool_context);
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
