#include "tools/coordinator/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "coordinator/coordinator-manager.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

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

            auto target = run_id.empty() ? to : run_id;
            tool_context.coordinator_manager->send_message(target, tool_context.agent_key, text);

            return nlohmann::json{{"sent", true}}.dump();
        }

    } // namespace

    void register_agent_send_message_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        registry.register_tool({.definition = {.name = "agent_send_message",
                                               .description = "Send a message to a running agent. Can address by run_id or agent name.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"run_id", {{"type", "string"}, {"description", "The run ID of the target agent"}}},
                                                                  {"to", {{"type", "string"}, {"description", "The agent name to send to (alternative to run_id)"}}},
                                                                  {"text", {{"type", "string"}, {"description", "The message text to send"}}}}},
                                                                {"required", nlohmann::json::array({"text"})}}},
                                .execute =
                                    [tool_context](const nlohmann::json &input) {
                                        return agent_send_message_handler(input, *tool_context);
                                    },
                                .deferred = true});
    }

} // namespace orangutan::tools
