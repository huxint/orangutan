#include "tools/task/task-tool.hpp"

#include <magic_enum/magic_enum.hpp>

#include "automation/cron-parser.hpp"
#include "automation/planner.hpp"
#include "automation/scheduler.hpp"
#include "tools/automation/automation-tool-support.hpp"
#include "tools/registry/contextual-tool-group.hpp"
#include "tools/registry/op-tool-support.hpp"
#include "tools/registry/schema-fragments.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-dispatch.hpp"
#include "tools/registry/tool-spec-builder.hpp"
#include "utils/format.hpp"

namespace orangutan::tools {
    namespace {

        using automation::task_schedule_kind;

        std::string format_task(const automation::TaskSpec &task) {
            std::string out;
            utils::format_to(out, "- {} [{}={}]", task.name, magic_enum::enum_name(task.schedule.kind), task.schedule.value);
            if (task.last_run_at.has_value()) {
                utils::format_to(out, " last_run={}", *task.last_run_at);
            }
            if (!task.last_status.empty()) {
                utils::format_to(out, " status={}", task.last_status);
            }
            return out;
        }

        std::string execute_task_tool(const nlohmann::json &input, const ToolRuntimeContext *ctx) {
            if (ctx == nullptr || ctx->automation_runtime == nullptr) {
                return "Error: task tool is not available in this context.";
            }

            const auto agent_key = ctx->agent_key.empty() ? std::string("default") : ctx->agent_key;
            auto &runtime = *ctx->automation_runtime;

            const auto run_add_or_update = [&runtime, &agent_key](const nlohmann::json &request, std::string_view op) {
                if (op == "update") {
                    if (const auto error = require_id_or_name(request); error.has_value()) {
                        return tool_dispatch::response{*error, true};
                    }
                }
                const auto id_or_name = request.value("id", request.value("name", ""));

                automation::TaskSpec task;
                if (op == "update") {
                    const auto existing = runtime.find_task(agent_key, id_or_name);
                    if (!existing.has_value()) {
                        return tool_dispatch::response{"Error: task not found.", true};
                    }
                    task = *existing;
                }

                task.agent_key = agent_key;
                task.id = op == "update" ? task.id : request.value("id", "");
                task.name = request.value("name", task.name);
                task.prompt = request.value("prompt", task.prompt);
                task.notes = request.value("notes", task.notes);
                task.enabled = request.contains("enabled") ? request.value("enabled", true) : task.enabled;

                const auto schedule_kind_value = request.value("schedule_kind", std::string{magic_enum::enum_name(task.schedule.kind)});
                const auto parsed_kind = magic_enum::enum_cast<task_schedule_kind>(schedule_kind_value);
                if (!parsed_kind.has_value()) {
                    return tool_dispatch::response{"Error: schedule_kind must be 'at' or 'cron'.", true};
                }
                task.schedule.kind = *parsed_kind;
                task.schedule.value = request.value("schedule", task.schedule.value);

                if (task.name.empty() || task.prompt.empty() || task.schedule.value.empty()) {
                    return tool_dispatch::response{"Error: name, schedule, and prompt are required.", true};
                }

                if (task.schedule.kind == task_schedule_kind::cron) {
                    if (!parse_cron(task.schedule.value).has_value()) {
                        return tool_dispatch::response{"Error: invalid cron schedule.", true};
                    }
                } else {
                    if (!automation::parse_absolute_time(task.schedule.value).has_value()) {
                        return tool_dispatch::response{"Error: invalid absolute schedule.", true};
                    }
                }

                auto delivery = builtin::detail::parse_delivery_overlay(request, task.delivery);
                if (!delivery.has_value()) {
                    return tool_dispatch::response{"Error: " + delivery.error() + ".", true};
                }
                task.delivery = std::move(*delivery);

                const auto task_id = runtime.save_task(task);
                return tool_dispatch::response{op == "add" ? "Added task '" + task.name + "' (" + task_id + ")." : "Updated task '" + task.name + "'."};
            };

            return dispatch_message(tool_dispatch()
                                        .unknown_op_error("Error: unknown operation. Supported: add, update, remove, list, run.")
                                        .on("list",
                                            [&runtime, &agent_key](const nlohmann::json &) {
                                                const auto tasks = runtime.list_tasks(agent_key);
                                                if (tasks.empty()) {
                                                    return tool_dispatch::response{"No tasks configured."};
                                                }
                                                std::string out;
                                                for (const auto &task : tasks) {
                                                    out.append(format_task(task));
                                                    out.push_back('\n');
                                                }
                                                return tool_dispatch::response{std::move(out)};
                                            })
                                        .on("remove",
                                            [&runtime, &agent_key](const nlohmann::json &request) {
                                                if (const auto error = require_id_or_name(request); error.has_value()) {
                                                    return tool_dispatch::response{*error, true};
                                                }
                                                const auto id_or_name = request.value("id", request.value("name", ""));
                                                return tool_dispatch::response{runtime.remove_task(agent_key, id_or_name) ? "Removed task." : "Error: task not found."};
                                            })
                                        .on("run",
                                            [&runtime, &agent_key](const nlohmann::json &request) {
                                                if (const auto error = require_id_or_name(request); error.has_value()) {
                                                    return tool_dispatch::response{*error, true};
                                                }
                                                const auto id_or_name = request.value("id", request.value("name", ""));
                                                return tool_dispatch::response{runtime.run_task_now(agent_key, id_or_name)};
                                            })
                                        .on("add",
                                            [&run_add_or_update](const nlohmann::json &request) {
                                                return run_add_or_update(request, "add");
                                            })
                                        .on("update",
                                            [&run_add_or_update](const nlohmann::json &request) {
                                                return run_add_or_update(request, "update");
                                            }),
                                    routed_input_with_default_op(input, ""));
        }

        PermissionResult check_task_permissions(const ToolUse &call) {
            const auto op = call.input.value("op", std::string{});
            if (op == "list") {
                return PermissionResult::allow();
            }
            return PermissionResult::ask("Task mutations require approval");
        }

    } // namespace

    void register_task_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        contextual_tool_group()
            .require_automation_runtime()
            .add(make_tool_spec_builder("task")
                     .description("Manage precise scheduled tasks for the current agent.")
                     .input_schema(schema_fragments::object_with_required(
                         {
                             {"op", schema_fragments::op_enum({"add", "update", "remove", "list", "run"})},
                             {"id", schema_fragments::id_field()},
                             {"name", {{"type", "string"}}},
                             {"schedule_kind", schema_fragments::op_enum({"at", "cron"})},
                             {"schedule", {{"type", "string"}}},
                             {"prompt", {{"type", "string"}}},
                             {"notes", {{"type", "string"}}},
                             {"enabled", {{"type", "boolean"}}},
                             {"delivery_mode", schema_fragments::delivery_mode_field()},
                             {"targets", schema_fragments::delivery_targets_field()},
                         },
                         {"op"}))
                     .check_permissions([](const ToolUse &call, const ToolPermissionContext &) {
                         return check_task_permissions(call);
                     })
                     .execute([tool_context](const nlohmann::json &input) {
                         return execute_task_tool(input, tool_context);
                     })
                     .deferred())
            .register_into(registry, tool_context);
    }

} // namespace orangutan::tools
