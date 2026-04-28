#include "tools/orchestration/register.hpp"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "orchestration/orchestration-manager.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-spec-builder.hpp"

namespace orangutan::tools {

    namespace {

        struct AgentStopToolContext {
            orchestration::OrchestrationManager *orchestration_manager = nullptr;
        };

        [[nodiscard]]
        auto make_agent_stop_tool_context(OrchestrationCapability capability) -> AgentStopToolContext {
            return AgentStopToolContext{
                .orchestration_manager = capability.orchestration_manager,
            };
        }

        auto agent_stop_handler(const nlohmann::json &input, const AgentStopToolContext &tool_context) -> std::string {
            auto run_id = input.at("run_id").get<std::string>();

            if (tool_context.orchestration_manager == nullptr) {
                return nlohmann::json{{"stopped", false}, {"error", "Orchestration manager not available"}}.dump();
            }

            tool_context.orchestration_manager->stop(run_id);

            auto run = tool_context.orchestration_manager->get_run(run_id);
            return nlohmann::json{
                {"stopped", true},
                {"run_id", run_id},
                {"status", run.has_value() ? "terminated" : "not_found"},
            }
                .dump();
        }

    } // namespace

    void register_agent_stop_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr) {
            return;
        }
        register_agent_stop_tool(registry, runtime_identity_context(*tool_context), orchestration_capability(*tool_context));
    }

    void register_agent_stop_tool(ToolRegistry &registry, RuntimeIdentityContext, OrchestrationCapability capability) {
        auto tool_context = make_agent_stop_tool_context(capability);
        if (auto tool = make_tool_spec_builder("agent_stop")
                            .description("Stop a running teammate. The teammate will be given a chance to clean up before being terminated.")
                            .input_schema({{"type", "object"},
                                           {"properties", {{"run_id", {{"type", "string"}, {"description", "The run ID of the agent to stop"}}}}},
                                           {"required", nlohmann::json::array({"run_id"})}})
                            .execute([tool_context = std::move(tool_context)](const nlohmann::json &input) {
                                return agent_stop_handler(input, tool_context);
                            })
                            .build();
            tool.has_value()) {
            registry.register_tool(std::move(*tool));
        } else {
            spdlog::warn("failed to register tool: {}", tool.error());
        }
    }

} // namespace orangutan::tools
