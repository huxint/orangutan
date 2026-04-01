#include "tools/register.hpp"
#include "subagent/subagent-manager.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <magic_enum/magic_enum.hpp>

namespace orangutan {
    namespace {

        nlohmann::json subagent_run_record_to_json(const SubagentRunRecord &run) {
            return nlohmann::json{
                {"run_id", run.run_id},
                {"parent_runtime_key", run.parent_runtime_key},
                {"parent_session_id", run.parent_session_id.has_value() ? nlohmann::json(*run.parent_session_id) : nlohmann::json(nullptr)},
                {"parent_agent_key", run.parent_agent_key},
                {"child_session_id", run.child_session_id},
                {"child_agent_key", run.child_agent_key},
                {"child_scope_key", run.child_scope_key},
                {"status", magic_enum::enum_name(run.status)},
                {"task_summary", run.task_summary},
                {"final_summary", run.final_summary},
                {"final_output", run.final_output},
                {"error_text", run.error_text},
                {"created_at", run.created_at},
                {"started_at", run.started_at.has_value() ? nlohmann::json(*run.started_at) : nlohmann::json(nullptr)},
                {"finished_at", run.finished_at.has_value() ? nlohmann::json(*run.finished_at) : nlohmann::json(nullptr)},
            };
        }

        std::optional<std::string> current_parent_session_id(const ToolRuntimeContext &tool_context) {
            if (tool_context.current_session_id == nullptr || tool_context.current_session_id->empty()) {
                return std::nullopt;
            }

            return *tool_context.current_session_id;
        }

        bool has_complete_runtime_identity(const ToolRuntimeContext &tool_context) {
            return !tool_context.runtime_key.empty() && !tool_context.agent_key.empty() && !tool_context.raw_caller_id.empty();
        }

        SubagentCallerContext make_subagent_caller_context(const ToolRuntimeContext &tool_context) {
            return SubagentCallerContext{
                .runtime_origin = tool_context.runtime_origin,
                .runtime_key = tool_context.runtime_key,
                .agent_key = tool_context.agent_key,
                .scope_key = tool_context.scope_key,
                .raw_caller_id = tool_context.raw_caller_id,
                .session_id = current_parent_session_id(tool_context),
                .allowed_child_agents = tool_context.allowed_child_agents,
                .is_child_run = tool_context.is_child_run,
                .approval_callback = tool_context.approval_callback,
            };
        }

        std::string spawn_subagent_tool(const nlohmann::json &input, const ToolRuntimeContext &tool_context) {
            auto request = SubagentSpawnRequest{
                .caller = make_subagent_caller_context(tool_context),
                .child_agent_key = input.at("child_agent_key").get<std::string>(),
                .task_summary = input.at("task_summary").get<std::string>(),
            };
            if (input.contains("child_scope_key")) {
                request.child_scope_key = input.at("child_scope_key").get<std::string>();
            }
            if (input.contains("child_session_id")) {
                request.child_session_id = input.at("child_session_id").get<std::string>();
            }

            const auto result = tool_context.subagent_manager->spawn(request);
            return nlohmann::json{{"accepted", result.accepted}, {"run_id", result.run_id}, {"error", result.error}}.dump();
        }

        std::string subagent_status_tool(const nlohmann::json &input, SubagentManager &subagent_manager, const ToolRuntimeContext &tool_context) {
            const auto result = subagent_manager.status(SubagentStatusRequest{
                .run_id = input.at("run_id").get<std::string>(),
                .caller = make_subagent_caller_context(tool_context),
            });
            return nlohmann::json{{"found", result.run.has_value()}, {"run", result.run.has_value() ? subagent_run_record_to_json(*result.run) : nlohmann::json(nullptr)}}.dump();
        }

        std::string subagent_wait_tool(const nlohmann::json &input, SubagentManager &subagent_manager, const ToolRuntimeContext &tool_context) {
            const auto timeout_ms = std::max(0, input.value("timeout_ms", 0));
            const auto result = subagent_manager.wait(SubagentWaitRequest{
                .run_id = input.at("run_id").get<std::string>(),
                .timeout = std::chrono::milliseconds{timeout_ms},
                .caller = make_subagent_caller_context(tool_context),
            });
            return nlohmann::json{{"state", magic_enum::enum_name(result.state)},
                                  {"run", result.run.has_value() ? subagent_run_record_to_json(*result.run) : nlohmann::json(nullptr)}}
                .dump();
        }

        bool should_register_subagent_tools(const ToolRuntimeContext *tool_context) {
            return tool_context != nullptr && has_complete_runtime_identity(*tool_context) && tool_context->subagent_manager != nullptr && !tool_context->is_child_run &&
                   !tool_context->allowed_child_agents.empty();
        }

    } // namespace

    void register_builtin_subagent_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (!should_register_subagent_tools(tool_context)) {
            return;
        }

        registry.register_tool({.definition = {.name = "subagent_spawn",
                                               .description = "Start an allowed child agent run and return structured run metadata.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"child_agent_key", {{"type", "string"}, {"description", "Allowed child agent key to run"}}},
                                                                  {"task_summary", {{"type", "string"}, {"description", "Summary of the work for the child run"}}}}},
                                                                {"required", nlohmann::json::array({"child_agent_key", "task_summary"})}}},
                                .execute = [tool_context](const nlohmann::json &input) {
                                    return spawn_subagent_tool(input, *tool_context);
                                }});

        registry.register_tool({.definition = {.name = "subagent_status",
                                               .description = "Inspect a subagent run and return structured status metadata.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties", {{"run_id", {{"type", "string"}, {"description", "Subagent run id"}}}}},
                                                                {"required", nlohmann::json::array({"run_id"})}}},
                                .execute = [tool_context](const nlohmann::json &input) {
                                    return subagent_status_tool(input, *tool_context->subagent_manager, *tool_context);
                                }});

        registry.register_tool({.definition = {.name = "subagent_wait",
                                               .description = "Wait for a subagent run and return structured status metadata.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"run_id", {{"type", "string"}, {"description", "Subagent run id"}}},
                                                                  {"timeout_ms", {{"type", "integer"}, {"description", "Maximum time to wait in milliseconds"}}}}},
                                                                {"required", nlohmann::json::array({"run_id"})}}},
                                .execute = [tool_context](const nlohmann::json &input) {
                                    return subagent_wait_tool(input, *tool_context->subagent_manager, *tool_context);
                                }});
    }

} // namespace orangutan
