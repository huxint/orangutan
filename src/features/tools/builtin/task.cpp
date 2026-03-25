#include "features/tools/builtin/task.hpp"

#include "features/automation/planner.hpp"
#include "features/automation/runtime.hpp"
#include "features/cron/parser.hpp"
#include "features/tools/builtin/automation-tool-support.hpp"

#include "infra/format.hpp"
#include <magic_enum/magic_enum.hpp>

namespace orangutan {
namespace {

using automation::TaskScheduleKind;

std::string format_task(const automation::TaskSpec &task) {
    std::string out;
    append(out, "- {} [{}={}]", task.name, magic_enum::enum_name(task.schedule.kind), task.schedule.value);
    if (task.last_run_at.has_value()) {
        append(out, " last_run={}", *task.last_run_at);
    }
    if (!task.last_status.empty()) {
        append(out, " status={}", task.last_status);
    }
    return out;
}

std::string execute_task_tool(const json &input, const ToolRuntimeContext *ctx) {
    if (ctx == nullptr || ctx->automation_runtime == nullptr) {
        return "Error: task tool is not available in this context.";
    }

    const auto op = input.value("op", "");
    const auto agent_key = ctx->agent_key.empty() ? std::string("default") : ctx->agent_key;
    auto &runtime = *ctx->automation_runtime;

    if (op == "list") {
        const auto tasks = runtime.list_tasks(agent_key);
        if (tasks.empty()) {
            return "No tasks configured.";
        }
        std::string out;
        for (const auto &task : tasks) {
            out.append(format_task(task));
            out.push_back('\n');
        }
        return out;
    }

    const auto id_or_name = input.value("id", input.value("name", ""));
    if ((op == "remove" || op == "run" || op == "update") && id_or_name.empty()) {
        return "Error: id or name is required.";
    }

    if (op == "remove") {
        return runtime.remove_task(agent_key, id_or_name) ? "Removed task." : "Error: task not found.";
    }
    if (op == "run") {
        return runtime.run_task_now(agent_key, id_or_name);
    }

    if (op != "add" && op != "update") {
        return "Error: unknown operation. Supported: add, update, remove, list, run.";
    }

    automation::TaskSpec task;
    if (op == "update") {
        const auto existing = runtime.find_task(agent_key, id_or_name);
        if (!existing.has_value()) {
            return "Error: task not found.";
        }
        task = *existing;
    }

    task.agent_key = agent_key;
    task.id = op == "update" ? task.id : input.value("id", "");
    task.name = input.value("name", task.name);
    task.prompt = input.value("prompt", task.prompt);
    task.notes = input.value("notes", task.notes);
    task.enabled = input.contains("enabled") ? input.value("enabled", true) : task.enabled;

    const auto schedule_kind_value = input.value("schedule_kind", std::string{magic_enum::enum_name(task.schedule.kind)});
    const auto parsed_kind = magic_enum::enum_cast<TaskScheduleKind>(schedule_kind_value);
    if (!parsed_kind.has_value()) {
        return "Error: schedule_kind must be 'at' or 'cron'.";
    }
    task.schedule.kind = *parsed_kind;
    task.schedule.value = input.value("schedule", task.schedule.value);

    if (task.name.empty() || task.prompt.empty() || task.schedule.value.empty()) {
        return "Error: name, schedule, and prompt are required.";
    }

    if (task.schedule.kind == TaskScheduleKind::cron) {
        if (!parse_cron(task.schedule.value).has_value()) {
            return "Error: invalid cron schedule.";
        }
    } else {
        if (!automation::parse_absolute_time(task.schedule.value).has_value()) {
            return "Error: invalid absolute schedule.";
        }
    }

    auto delivery = builtin::detail::parse_delivery_overlay(input, task.delivery);
    if (!delivery.has_value()) {
        return "Error: " + delivery.error() + ".";
    }
    task.delivery = std::move(*delivery);

    const auto task_id = runtime.save_task(task);
    return op == "add" ? "Added task '" + task.name + "' (" + task_id + ")." : "Updated task '" + task.name + "'.";
}

} // namespace

void register_task_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
    if (tool_context == nullptr || tool_context->automation_runtime == nullptr) {
        return;
    }

    registry.register_tool({
        .definition =
            {
                .name = "task",
                .description = "Manage precise scheduled tasks for the current agent.",
                .input_schema =
                    {
                        {"type", "object"},
                        {"properties",
                         {
                             {"op", {{"type", "string"}, {"enum", json::array({"add", "update", "remove", "list", "run"})}}},
                             {"id", {{"type", "string"}}},
                             {"name", {{"type", "string"}}},
                             {"schedule_kind", {{"type", "string"}, {"enum", json::array({"at", "cron"})}}},
                             {"schedule", {{"type", "string"}}},
                             {"prompt", {{"type", "string"}}},
                             {"notes", {{"type", "string"}}},
                             {"enabled", {{"type", "boolean"}}},
                             {"delivery_mode", {{"type", "string"}, {"enum", json::array({"silent", "notify"})}}},
                             {"targets", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                         }},
                        {"required", json::array({"op"})},
                    },
            },
        .execute =
            [tool_context](const json &input) {
                return execute_task_tool(input, tool_context);
            },
    });
}

} // namespace orangutan
