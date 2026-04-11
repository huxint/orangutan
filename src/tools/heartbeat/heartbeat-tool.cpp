#include "tools/heartbeat/heartbeat-tool.hpp"

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

            const auto agent_key = builtin::detail::resolve_agent_key(*ctx);
            auto &runtime = *ctx->automation_runtime;

            const auto run_add_or_update = [&runtime, &agent_key](const nlohmann::json &request, std::string_view op) {
                const auto entity_key = builtin::detail::id_or_name(request);
                if (op == "update" && entity_key.empty()) {
                    return tool_dispatch::response{.message = "Error: id or name is required.", .is_error = true};
                }

                automation::HeartbeatSpec heartbeat;
                if (op == "update") {
                    const auto existing = runtime.find_heartbeat(agent_key, entity_key);
                    if (!existing.has_value()) {
                        return tool_dispatch::response{.message = "Error: heartbeat not found.", .is_error = true};
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
                        return tool_dispatch::response{.message = "Error: invalid heartbeat interval.", .is_error = true};
                    }
                    heartbeat.every_seconds = *parsed;
                }
                if (const auto it = request.find("jitter"); it != request.end() && it->is_string()) {
                    const auto parsed = automation::parse_duration_seconds(it->get<std::string>());
                    if (!parsed.has_value()) {
                        return tool_dispatch::response{.message = "Error: invalid heartbeat jitter.", .is_error = true};
                    }
                    heartbeat.jitter_seconds = *parsed;
                }

                if (heartbeat.name.empty() || heartbeat.prompt.empty() || heartbeat.every_seconds <= 0) {
                    return tool_dispatch::response{.message = "Error: name, prompt, and every are required.", .is_error = true};
                }

                const bool has_active_hours_field = request.contains("active_hours");
                const bool timing_changed = request.contains("every") || request.contains("jitter") || has_active_hours_field;

                auto delivery = builtin::detail::parse_delivery_overlay(request, heartbeat.delivery);
                if (!delivery.has_value()) {
                    return tool_dispatch::response{.message = "Error: " + delivery.error() + ".", .is_error = true};
                }
                heartbeat.delivery = std::move(*delivery);

                auto active_hours = builtin::detail::parse_active_hours_overlay(request);
                if (!active_hours.has_value()) {
                    return tool_dispatch::response{.message = "Error: " + active_hours.error() + ".", .is_error = true};
                }
                if (active_hours->has_value()) {
                    heartbeat.active_hours = std::move(**active_hours);
                }
                if (op == "add" || !heartbeat.next_due_at.has_value() || timing_changed) {
                    heartbeat.next_due_at = automation::plan_next_heartbeat_due(heartbeat, automation::Clock::now());
                }

                const auto heartbeat_id = runtime.save_heartbeat(heartbeat);
                return tool_dispatch::response{.message = op == "add" ? "Added heartbeat '" + heartbeat.name + "' (" + heartbeat_id + ")."
                                                                      : "Updated heartbeat '" + heartbeat.name + "'."};
            };

            return dispatch_message(
                tool_dispatch()
                    .unknown_op_error("Error: unknown operation. Supported: add, update, remove, list, run, pause, resume.")
                    .on("list",
                        [&runtime, &agent_key](const nlohmann::json &) {
                            const auto heartbeats = runtime.list_heartbeats(agent_key);
                            if (heartbeats.empty()) {
                                return tool_dispatch::response{.message = "No heartbeats configured."};
                            }
                            std::string out;
                            for (const auto &heartbeat : heartbeats) {
                                out.append(format_heartbeat(heartbeat));
                                out.push_back('\n');
                            }
                            return tool_dispatch::response{.message = std::move(out)};
                        })
                    .on("remove",
                        [&runtime, &agent_key](const nlohmann::json &request) {
                            return builtin::detail::require_named_entity(request, "Error: id or name is required.", [&](const std::string &entity_key) {
                                return tool_dispatch::response{.message = runtime.remove_heartbeat(agent_key, entity_key) ? "Removed heartbeat." : "Error: heartbeat not found."};
                            });
                        })
                    .on("run",
                        [&runtime, &agent_key](const nlohmann::json &request) {
                            return builtin::detail::require_named_entity(request, "Error: id or name is required.", [&](const std::string &entity_key) {
                                return tool_dispatch::response{.message = runtime.run_heartbeat_now(agent_key, entity_key)};
                            });
                        })
                    .on("pause",
                        [&runtime, &agent_key](const nlohmann::json &request) {
                            return builtin::detail::require_named_entity(request, "Error: id or name is required.", [&](const std::string &entity_key) {
                                return tool_dispatch::response{.message =
                                                                   runtime.pause_heartbeat(agent_key, entity_key, true) ? "Paused heartbeat." : "Error: heartbeat not found."};
                            });
                        })
                    .on("resume",
                        [&runtime, &agent_key](const nlohmann::json &request) {
                            return builtin::detail::require_named_entity(request, "Error: id or name is required.", [&](const std::string &entity_key) {
                                return tool_dispatch::response{.message =
                                                                   runtime.pause_heartbeat(agent_key, entity_key, false) ? "Resumed heartbeat." : "Error: heartbeat not found."};
                            });
                        })
                    .on("add",
                        [&run_add_or_update](const nlohmann::json &request) {
                            return run_add_or_update(request, "add");
                        })
                    .on("update",
                        [&run_add_or_update](const nlohmann::json &request) {
                            return run_add_or_update(request, "update");
                        }),
                builtin::detail::normalize_automation_op_input(input));
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
