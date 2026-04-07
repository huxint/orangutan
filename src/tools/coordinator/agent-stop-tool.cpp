#include "tools/coordinator/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"
#include "coordinator/coordinator-manager.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace orangutan::tools {

    namespace {

        std::string agent_stop_handler(const nlohmann::json &input, const ToolRuntimeContext &tool_context) {
            auto run_id = input.at("run_id").get<std::string>();

            if (tool_context.coordinator_manager == nullptr) {
                return nlohmann::json{{"stopped", false}, {"error", "Coordinator manager not available"}}.dump();
            }

            tool_context.coordinator_manager->stop(run_id);

            auto run = tool_context.coordinator_manager->get_run(run_id);
            return nlohmann::json{
                {"stopped", true},
                {"run_id", run_id},
                {"status", run.has_value() ? "terminated" : "not_found"},
            }
                .dump();
        }

    } // namespace

    void register_agent_stop_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        registry.register_tool(tool_spec_builder("agent_stop")
                                   .description("Stop a running agent. The agent will be given a chance to clean up before being terminated.")
                                   .input_schema({{"type", "object"},
                                                  {"properties", {{"run_id", {{"type", "string"}, {"description", "The run ID of the agent to stop"}}}}},
                                                  {"required", nlohmann::json::array({"run_id"})}})
                                   .execute([tool_context](const nlohmann::json &input) {
                                       return agent_stop_handler(input, *tool_context);
                                   })
                                   .deferred()
                                   .build());
    }

} // namespace orangutan::tools
