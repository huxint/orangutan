#include "tools/heartbeat/heartbeat-tool.hpp"

#include "automation/planner.hpp"
#include "automation/scheduler.hpp"
#include "tools/automation/automation-tool-support.hpp"

#include "infra/format.hpp"

namespace orangutan {
    namespace {

        std::string format_heartbeat(const automation::HeartbeatSpec &heartbeat) {
            std::string out;
            append(out, "- {} every={}s", heartbeat.name, heartbeat.every_seconds);
            if (heartbeat.jitter_seconds > 0) {
                append(out, " jitter={}s", heartbeat.jitter_seconds);
            }
            if (heartbeat.next_due_at.has_value()) {
                append(out, " next_due={}", *heartbeat.next_due_at);
            }
            if (heartbeat.paused) {
                out.append(" paused");
            }
            return out;
        }

        std::string execute_heartbeat_tool(const nlohmann::json &input, const ToolRuntimeContext *ctx) {
            if (ctx == nullptr || ctx->automation_runtime == nullptr) {
                return "Error: heartbeat tool is not available in this context.";
            }

            const auto op = input.value("op", "");
            const auto agent_key = ctx->agent_key.empty() ? std::string("default") : ctx->agent_key;
            auto &runtime = *ctx->automation_runtime;

            if (op == "list") {
                const auto heartbeats = runtime.list_heartbeats(agent_key);
                if (heartbeats.empty()) {
                    return "No heartbeats configured.";
                }
                std::string out;
                for (const auto &heartbeat : heartbeats) {
                    out.append(format_heartbeat(heartbeat));
                    out.push_back('\n');
                }
                return out;
            }

            const auto id_or_name = input.value("id", input.value("name", ""));
            if ((op == "remove" || op == "run" || op == "pause" || op == "resume" || op == "update") && id_or_name.empty()) {
                return "Error: id or name is required.";
            }

            if (op == "remove") {
                return runtime.remove_heartbeat(agent_key, id_or_name) ? "Removed heartbeat." : "Error: heartbeat not found.";
            }
            if (op == "run") {
                return runtime.run_heartbeat_now(agent_key, id_or_name);
            }
            if (op == "pause") {
                return runtime.pause_heartbeat(agent_key, id_or_name, true) ? "Paused heartbeat." : "Error: heartbeat not found.";
            }
            if (op == "resume") {
                return runtime.pause_heartbeat(agent_key, id_or_name, false) ? "Resumed heartbeat." : "Error: heartbeat not found.";
            }

            if (op != "add" && op != "update") {
                return "Error: unknown operation. Supported: add, update, remove, list, run, pause, resume.";
            }

            automation::HeartbeatSpec heartbeat;
            if (op == "update") {
                const auto existing = runtime.find_heartbeat(agent_key, id_or_name);
                if (!existing.has_value()) {
                    return "Error: heartbeat not found.";
                }
                heartbeat = *existing;
            }

            heartbeat.agent_key = agent_key;
            heartbeat.id = op == "update" ? heartbeat.id : input.value("id", "");
            heartbeat.name = input.value("name", heartbeat.name);
            heartbeat.prompt = input.value("prompt", heartbeat.prompt);
            heartbeat.notes = input.value("notes", heartbeat.notes);
            heartbeat.enabled = input.contains("enabled") ? input.value("enabled", true) : heartbeat.enabled;
            heartbeat.paused = input.contains("paused") ? input.value("paused", heartbeat.paused) : heartbeat.paused;

            if (const auto it = input.find("every"); it != input.end() && it->is_string()) {
                const auto parsed = automation::parse_duration_seconds(it->get<std::string>());
                if (!parsed.has_value()) {
                    return "Error: invalid heartbeat interval.";
                }
                heartbeat.every_seconds = *parsed;
            }
            if (const auto it = input.find("jitter"); it != input.end() && it->is_string()) {
                const auto parsed = automation::parse_duration_seconds(it->get<std::string>());
                if (!parsed.has_value()) {
                    return "Error: invalid heartbeat jitter.";
                }
                heartbeat.jitter_seconds = *parsed;
            }

            if (heartbeat.name.empty() || heartbeat.prompt.empty() || heartbeat.every_seconds <= 0) {
                return "Error: name, prompt, and every are required.";
            }

            const bool has_active_hours_field = input.contains("active_hours");
            const bool timing_changed = input.contains("every") || input.contains("jitter") || has_active_hours_field;

            auto delivery = builtin::detail::parse_delivery_overlay(input, heartbeat.delivery);
            if (!delivery.has_value()) {
                return "Error: " + delivery.error() + ".";
            }
            heartbeat.delivery = std::move(*delivery);

            auto active_hours = builtin::detail::parse_active_hours_overlay(input);
            if (!active_hours.has_value()) {
                return "Error: " + active_hours.error() + ".";
            }
            if (active_hours->has_value()) {
                heartbeat.active_hours = std::move(**active_hours);
            }
            if (op == "add" || !heartbeat.next_due_at.has_value() || timing_changed) {
                heartbeat.next_due_at = automation::plan_next_heartbeat_due(heartbeat, automation::Clock::now());
            }

            const auto heartbeat_id = runtime.save_heartbeat(heartbeat);
            return op == "add" ? "Added heartbeat '" + heartbeat.name + "' (" + heartbeat_id + ")." : "Updated heartbeat '" + heartbeat.name + "'.";
        }

    } // namespace

    void register_heartbeat_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr || tool_context->automation_runtime == nullptr) {
            return;
        }

        registry.register_tool({
            .definition =
                {
                    .name = "heartbeat",
                    .description = "Manage approximate periodic heartbeats for the current agent.",
                    .input_schema =
                        {
                            {"type", "object"},
                            {"properties",
                             {
                                 {"op", {{"type", "string"}, {"enum", nlohmann::json::array({"add", "update", "remove", "list", "run", "pause", "resume"})}}},
                                 {"id", {{"type", "string"}}},
                                 {"name", {{"type", "string"}}},
                                 {"every", {{"type", "string"}}},
                                 {"jitter", {{"type", "string"}}},
                                 {"prompt", {{"type", "string"}}},
                                 {"notes", {{"type", "string"}}},
                                 {"enabled", {{"type", "boolean"}}},
                                 {"paused", {{"type", "boolean"}}},
                                 {"delivery_mode", {{"type", "string"}, {"enum", nlohmann::json::array({"silent", "notify"})}}},
                                 {"targets", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                 {"active_hours", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                             }},
                            {"required", nlohmann::json::array({"op"})},
                        },
                },
            .execute =
                [tool_context](const nlohmann::json &input) {
                    return execute_heartbeat_tool(input, tool_context);
                },
        });
    }

} // namespace orangutan
