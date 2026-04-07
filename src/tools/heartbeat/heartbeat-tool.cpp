#include "tools/heartbeat/heartbeat-tool.hpp"

#include "automation/planner.hpp"
#include "automation/scheduler.hpp"

#include "tools/automation/automation-tool-support.hpp"
#include "tools/registry/contextual-tool-group.hpp"
#include "tools/registry/schema-fragments.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-dispatch.hpp"
#include "tools/registry/tool-spec-builder.hpp"
#include "utils/format.hpp"

namespace orangutan::tools {
    namespace {

        std::string format_heartbeat(const automation::HeartbeatSpec &heartbeat) {
            std::string out;
            utils::format_to(out, "- {} every={}s", heartbeat.name, heartbeat.every_seconds);
            if (heartbeat.jitter_seconds > 0) {
                utils::format_to(out, " jitter={}s", heartbeat.jitter_seconds);
            }
            if (heartbeat.next_due_at.has_value()) {
                utils::format_to(out, " next_due={}", *heartbeat.next_due_at);
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

            const auto agent_key = ctx->agent_key.empty() ? std::string("default") : ctx->agent_key;
            auto &runtime = *ctx->automation_runtime;

            const auto run_add_or_update = [&runtime, &agent_key](const nlohmann::json &request, std::string_view op) {
                const auto id_or_name = request.value("id", request.value("name", ""));
                if (op == "update" && id_or_name.empty()) {
                    return tool_dispatch::response{"Error: id or name is required.", true};
                }

                automation::HeartbeatSpec heartbeat;
                if (op == "update") {
                    const auto existing = runtime.find_heartbeat(agent_key, id_or_name);
                    if (!existing.has_value()) {
                        return tool_dispatch::response{"Error: heartbeat not found.", true};
                    }
                    heartbeat = *existing;
                }

                heartbeat.agent_key = agent_key;
                heartbeat.id = op == "update" ? heartbeat.id : request.value("id", "");
                heartbeat.name = request.value("name", heartbeat.name);
                heartbeat.prompt = request.value("prompt", heartbeat.prompt);
                heartbeat.notes = request.value("notes", heartbeat.notes);
                heartbeat.enabled = request.contains("enabled") ? request.value("enabled", true) : heartbeat.enabled;
                heartbeat.paused = request.contains("paused") ? request.value("paused", heartbeat.paused) : heartbeat.paused;

                if (const auto it = request.find("every"); it != request.end() && it->is_string()) {
                    const auto parsed = automation::parse_duration_seconds(it->get<std::string>());
                    if (!parsed.has_value()) {
                        return tool_dispatch::response{"Error: invalid heartbeat interval.", true};
                    }
                    heartbeat.every_seconds = *parsed;
                }
                if (const auto it = request.find("jitter"); it != request.end() && it->is_string()) {
                    const auto parsed = automation::parse_duration_seconds(it->get<std::string>());
                    if (!parsed.has_value()) {
                        return tool_dispatch::response{"Error: invalid heartbeat jitter.", true};
                    }
                    heartbeat.jitter_seconds = *parsed;
                }

                if (heartbeat.name.empty() || heartbeat.prompt.empty() || heartbeat.every_seconds <= 0) {
                    return tool_dispatch::response{"Error: name, prompt, and every are required.", true};
                }

                const bool has_active_hours_field = request.contains("active_hours");
                const bool timing_changed = request.contains("every") || request.contains("jitter") || has_active_hours_field;

                auto delivery = builtin::detail::parse_delivery_overlay(request, heartbeat.delivery);
                if (!delivery.has_value()) {
                    return tool_dispatch::response{"Error: " + delivery.error() + ".", true};
                }
                heartbeat.delivery = std::move(*delivery);

                auto active_hours = builtin::detail::parse_active_hours_overlay(request);
                if (!active_hours.has_value()) {
                    return tool_dispatch::response{"Error: " + active_hours.error() + ".", true};
                }
                if (active_hours->has_value()) {
                    heartbeat.active_hours = std::move(**active_hours);
                }
                if (op == "add" || !heartbeat.next_due_at.has_value() || timing_changed) {
                    heartbeat.next_due_at = automation::plan_next_heartbeat_due(heartbeat, automation::Clock::now());
                }

                const auto heartbeat_id = runtime.save_heartbeat(heartbeat);
                return tool_dispatch::response{op == "add" ? "Added heartbeat '" + heartbeat.name + "' (" + heartbeat_id + ")." : "Updated heartbeat '" + heartbeat.name + "'."};
            };

            const auto normalized_op = input.value("op", "");
            auto routed_input = input;
            routed_input["op"] = normalized_op;

            const auto result =
                tool_dispatch()
                    .unknown_op_error("Error: unknown operation. Supported: add, update, remove, list, run, pause, resume.")
                    .on("list",
                        [&runtime, &agent_key](const nlohmann::json &) {
                            const auto heartbeats = runtime.list_heartbeats(agent_key);
                            if (heartbeats.empty()) {
                                return tool_dispatch::response{"No heartbeats configured."};
                            }
                            std::string out;
                            for (const auto &heartbeat : heartbeats) {
                                out.append(format_heartbeat(heartbeat));
                                out.push_back('\n');
                            }
                            return tool_dispatch::response{std::move(out)};
                        })
                    .on("remove",
                        [&runtime, &agent_key](const nlohmann::json &request) {
                            const auto id_or_name = request.value("id", request.value("name", ""));
                            if (id_or_name.empty()) {
                                return tool_dispatch::response{"Error: id or name is required.", true};
                            }
                            return tool_dispatch::response{runtime.remove_heartbeat(agent_key, id_or_name) ? "Removed heartbeat." : "Error: heartbeat not found."};
                        })
                    .on("run",
                        [&runtime, &agent_key](const nlohmann::json &request) {
                            const auto id_or_name = request.value("id", request.value("name", ""));
                            if (id_or_name.empty()) {
                                return tool_dispatch::response{"Error: id or name is required.", true};
                            }
                            return tool_dispatch::response{runtime.run_heartbeat_now(agent_key, id_or_name)};
                        })
                    .on("pause",
                        [&runtime, &agent_key](const nlohmann::json &request) {
                            const auto id_or_name = request.value("id", request.value("name", ""));
                            if (id_or_name.empty()) {
                                return tool_dispatch::response{"Error: id or name is required.", true};
                            }
                            return tool_dispatch::response{runtime.pause_heartbeat(agent_key, id_or_name, true) ? "Paused heartbeat." : "Error: heartbeat not found."};
                        })
                    .on("resume",
                        [&runtime, &agent_key](const nlohmann::json &request) {
                            const auto id_or_name = request.value("id", request.value("name", ""));
                            if (id_or_name.empty()) {
                                return tool_dispatch::response{"Error: id or name is required.", true};
                            }
                            return tool_dispatch::response{runtime.pause_heartbeat(agent_key, id_or_name, false) ? "Resumed heartbeat." : "Error: heartbeat not found."};
                        })
                    .on("add",
                        [&run_add_or_update](const nlohmann::json &request) {
                            return run_add_or_update(request, "add");
                        })
                    .on("update",
                        [&run_add_or_update](const nlohmann::json &request) {
                            return run_add_or_update(request, "update");
                        })
                    .run(routed_input);

            return result.message;
        }

    } // namespace

    void register_heartbeat_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        contextual_tool_group()
            .require_automation_runtime()
            .add(make_tool_spec_builder("heartbeat")
                     .description("Manage approximate periodic heartbeats for the current agent.")
                     .input_schema(schema_fragments::object_with_required(
                         {
                             {"op", schema_fragments::op_enum({"add", "update", "remove", "list", "run", "pause", "resume"})},
                             {"id", schema_fragments::id_field()},
                             {"name", {{"type", "string"}}},
                             {"every", {{"type", "string"}}},
                             {"jitter", {{"type", "string"}}},
                             {"prompt", {{"type", "string"}}},
                             {"notes", {{"type", "string"}}},
                             {"enabled", {{"type", "boolean"}}},
                             {"paused", {{"type", "boolean"}}},
                             {"delivery_mode", schema_fragments::delivery_mode_field()},
                             {"targets", schema_fragments::delivery_targets_field()},
                             {"active_hours", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                         },
                         {"op"}))
                     .execute([tool_context](const nlohmann::json &input) {
                         return execute_heartbeat_tool(input, tool_context);
                     })
                     .deferred())
            .register_into(registry, tool_context);
    }

} // namespace orangutan::tools
