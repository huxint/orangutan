#include "features/tools/builtin/cron.hpp"
#include "features/cron/parser.hpp"
#include "features/cron/store.hpp"
#include "features/heartbeat/scheduler.hpp"

#include <spdlog/spdlog.h>

namespace orangutan {

namespace {

std::string format_next_fire(const CronExpr &expr) {
    auto next = next_fire_time(expr, std::chrono::system_clock::now());
    if (!next.has_value()) {
        return "unknown";
    }
    auto time = std::chrono::system_clock::to_time_t(*next);
    std::tm tm{};
    localtime_r(&time, &tm);
    std::array<char, 64> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M", &tm);
    return {buf.data()};
}

std::string handle_cron_add(const json &input, CronStore *store, HeartbeatScheduler *scheduler) {
    auto name = input.value("name", "");
    auto cron = input.value("cron", "");
    auto prompt = input.value("prompt", "");
    auto agent = input.value("agent", "default");
    auto channel = input.value("channel", "cli");

    if (name.empty() || cron.empty() || prompt.empty()) {
        return "Error: name, cron, and prompt are required for 'add' operation.";
    }

    auto expr = parse_cron(cron);
    if (!expr.has_value()) {
        return "Error: invalid cron expression '" + cron + "'.";
    }

    if (scheduler->has_job(name)) {
        return "Error: a job named '" + name + "' already exists.";
    }

    if (!store->add({.name = name, .cron = cron, .prompt = prompt, .agent = agent, .channel = channel})) {
        return "Error: failed to persist job '" + name + "'.";
    }

    scheduler->add_job(name, *expr, agent, channel, prompt, true);

    auto next = format_next_fire(*expr);
    return "Added cron job '" + name + "' [" + cron + "]. Next fire: " + next;
}

std::string handle_cron_remove(const json &input, CronStore *store, HeartbeatScheduler *scheduler) {
    auto name = input.value("name", "");
    if (name.empty()) {
        return "Error: name is required for 'remove' operation.";
    }

    if (!scheduler->has_job(name)) {
        return "Error: no job named '" + name + "' exists.";
    }

    if (!scheduler->remove_job(name)) {
        return "Error: job '" + name + "' is a static (config) job and cannot be removed at runtime.";
    }

    store->remove(name);
    return "Removed cron job '" + name + "'.";
}

std::string handle_cron_list(HeartbeatScheduler *scheduler) {
    auto jobs = scheduler->jobs();
    if (jobs.empty()) {
        return "No cron jobs configured.";
    }

    std::string result;
    for (const auto &job : jobs) {
        auto next = format_next_fire(job.cron_expr);
        auto source = job.dynamic ? "dynamic" : "config";
        result += "- " + job.name + " [" + source + "] next: " + next + "\n";
        result += "  agent=" + job.agent + " channel=" + job.channel + "\n";
    }
    return result;
}

std::string handle_cron_run(const json &input, HeartbeatScheduler *scheduler) {
    auto name = input.value("name", "");
    if (name.empty()) {
        return "Error: name is required for 'run' operation.";
    }

    if (!scheduler->fire_job(name)) {
        return "Error: no job named '" + name + "' exists.";
    }

    return "Fired job '" + name + "' immediately.";
}

std::string execute_cron_tool(const json &input, const ToolRuntimeContext *ctx) {
    if (ctx == nullptr || ctx->cron_store == nullptr || ctx->heartbeat_scheduler == nullptr) {
        return "Error: cron tool is not available in this context.";
    }

    auto op = input.value("op", "");
    if (op == "add") {
        return handle_cron_add(input, ctx->cron_store, ctx->heartbeat_scheduler);
    }
    if (op == "remove") {
        return handle_cron_remove(input, ctx->cron_store, ctx->heartbeat_scheduler);
    }
    if (op == "list") {
        return handle_cron_list(ctx->heartbeat_scheduler);
    }
    if (op == "run") {
        return handle_cron_run(input, ctx->heartbeat_scheduler);
    }
    return "Error: unknown operation '" + op + "'. Supported: add, remove, list, run.";
}

} // namespace

void register_cron_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
    if (tool_context == nullptr || tool_context->heartbeat_scheduler == nullptr) {
        return;
    }

    registry.register_tool({
        .definition = {
            .name = "cron",
            .description = "Manage cron-based heartbeat jobs at runtime. Operations: add, remove, list, run.",
            .input_schema = {
                {"type", "object"},
                {"properties", {
                    {"op", {{"type", "string"}, {"description", "Operation: add, remove, list, run"}, {"enum", json::array({"add", "remove", "list", "run"})}}},
                    {"name", {{"type", "string"}, {"description", "Job name (required for add/remove/run)"}}},
                    {"cron", {{"type", "string"}, {"description", "Cron expression, e.g. '*/5 * * * *' (required for add)"}}},
                    {"prompt", {{"type", "string"}, {"description", "Prompt text for the heartbeat job (required for add)"}}},
                    {"agent", {{"type", "string"}, {"description", "Agent to use (default: 'default')"}}},
                    {"channel", {{"type", "string"}, {"description", "Reply channel (default: 'cli')"}}},
                }},
                {"required", json::array({"op"})},
            },
        },
        .execute = [tool_context](const json &input) {
            return execute_cron_tool(input, tool_context);
        },
    });
}

} // namespace orangutan
