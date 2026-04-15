#include "tools/automation/automation-tool.hpp"

#include "automation/service.hpp"
#include "tools/automation/automation-tool-support.hpp"
#include "tools/registry/contextual-tool-group.hpp"
#include "tools/registry/op-tool-support.hpp"
#include "tools/registry/schema-fragments.hpp"
#include "tools/registry/tool-dispatch.hpp"
#include "tools/registry/tool-spec-builder.hpp"

#include <nlohmann/json.hpp>

namespace orangutan::tools {
    namespace {

        [[nodiscard]]
        nlohmann::json unix_time_to_json(const std::optional<base::i64> &value) {
            if (!value.has_value()) {
                return nullptr;
            }
            return *value;
        }

        [[nodiscard]]
        nlohmann::json automation_to_json(const automation::Automation &automation) {
            return {
                {"id", automation.id},
                {"agent_key", automation.agent_key},
                {"name", automation.name},
                {"prompt", automation.prompt},
                {"notes", automation.notes},
                {"enabled", automation.enabled},
                {"paused", automation.paused},
                {"trigger", automation::trigger_to_json(automation.trigger)},
                {"delivery", automation::delivery_policy_to_json(automation.delivery)},
                {"tags", automation.tags},
                {"last_run_at", unix_time_to_json(automation.last_run_at)},
                {"next_due_at", unix_time_to_json(automation.next_due_at)},
                {"last_status", automation.last_status},
            };
        }

        [[nodiscard]]
        nlohmann::json run_to_json(const automation::RunRecord &run) {
            return {
                {"id", run.id},
                {"automation_id", run.automation_id},
                {"agent_key", run.agent_key},
                {"automation_name", run.automation_name},
                {"started_at", run.started_at},
                {"finished_at", unix_time_to_json(run.finished_at)},
                {"status", run.status},
                {"summary", run.summary},
                {"reply", run.reply},
                {"delivery_status", run.delivery_status},
                {"log_path", run.log_path},
            };
        }

        [[nodiscard]]
        nlohmann::json delivery_to_json(const automation::DeliveryRecord &delivery) {
            return {
                {"id", delivery.id},
                {"run_id", delivery.run_id},
                {"automation_id", delivery.automation_id},
                {"agent_key", delivery.agent_key},
                {"target", delivery.target},
                {"status", delivery.status},
                {"title", delivery.title},
                {"body", delivery.body},
                {"created_at", delivery.created_at},
                {"acked_at", unix_time_to_json(delivery.acked_at)},
            };
        }

        [[nodiscard]]
        std::expected<automation::RunQuery, std::string> parse_run_query(const nlohmann::json &request, const ToolRuntimeContext *ctx, const automation::AutomationService &service) {
            automation::RunQuery query;
            query.agent_key = builtin::detail::resolve_query_agent_key(ctx, request);
            if (const auto it = request.find("automation_id"); it != request.end() && it->is_string()) {
                query.automation_id = it->get<std::string>();
                return query;
            }

            const auto selector = builtin::detail::id_or_name(request);
            if (selector.empty()) {
                return query;
            }

            if (query.agent_key.empty()) {
                return std::unexpected("agent_key is required when selecting automation runs by id or name");
            }

            const auto automation = service.find(query.agent_key, selector);
            if (!automation.has_value()) {
                return std::unexpected("automation not found");
            }
            query.automation_id = automation->id;
            return query;
        }

        [[nodiscard]]
        std::expected<automation::DeliveryQuery, std::string> parse_delivery_query(const nlohmann::json &request, const ToolRuntimeContext *ctx,
                                                                                   const automation::AutomationService &service) {
            automation::DeliveryQuery query;
            query.agent_key = builtin::detail::resolve_query_agent_key(ctx, request);
            query.run_id = request.value("run_id", "");
            query.target = request.value("target", "");
            query.only_unacked = request.value("only_unacked", false);

            if (const auto it = request.find("automation_id"); it != request.end() && it->is_string()) {
                query.automation_id = it->get<std::string>();
                return query;
            }

            const auto selector = builtin::detail::id_or_name(request);
            if (selector.empty()) {
                return query;
            }

            if (query.agent_key.empty()) {
                return std::unexpected("agent_key is required when selecting deliveries by id or name");
            }

            const auto automation = service.find(query.agent_key, selector);
            if (!automation.has_value()) {
                return std::unexpected("automation not found");
            }
            query.automation_id = automation->id;
            return query;
        }

        std::string execute_automation_tool(const nlohmann::json &input, const ToolRuntimeContext *ctx) {
            if (ctx == nullptr || ctx->automation_service == nullptr) {
                return "Error: automation tool is not available in this context.";
            }

            auto &service = *ctx->automation_service;
            const auto default_agent_key = builtin::detail::resolve_agent_key(*ctx);

            const auto run_create_or_update = [&service, &default_agent_key](const nlohmann::json &request, std::string_view op) {
                try {
                    if (op == "update") {
                        if (const auto error = require_id_or_name(request); error.has_value()) {
                            return tool_dispatch::response{.message = *error, .is_error = true};
                        }

                        const auto existing = service.find(default_agent_key, builtin::detail::id_or_name(request));
                        if (!existing.has_value()) {
                            return tool_dispatch::response{.message = "Error: automation not found.", .is_error = true};
                        }

                        const auto updated = builtin::detail::apply_update_request(request, *existing, default_agent_key);
                        if (!updated.has_value()) {
                            return tool_dispatch::response{.message = "Error: " + updated.error() + ".", .is_error = true};
                        }

                        static_cast<void>(service.save(*updated));
                        return tool_dispatch::response{.message = "Updated automation '" + updated->name + "'."};
                    }

                    const auto created = builtin::detail::parse_create_request(request, default_agent_key);
                    if (!created.has_value()) {
                        return tool_dispatch::response{.message = "Error: " + created.error() + ".", .is_error = true};
                    }

                    const auto id = service.save(*created);
                    return tool_dispatch::response{.message = "Created automation '" + created->name + "' (" + id + ")."};
                } catch (const std::exception &error) {
                    return tool_dispatch::response{.message = "Error: " + std::string(error.what()) + ".", .is_error = true};
                }
            };

            return dispatch_message(
                tool_dispatch()
                    .unknown_op_error("Error: unknown operation. Supported: create, update, get, list, remove, run, pause, resume, list_runs, list_deliveries, ack_delivery, clear_deliveries.")
                    .on("create",
                        [&run_create_or_update](const nlohmann::json &request) {
                            return run_create_or_update(request, "create");
                        })
                    .on("update",
                        [&run_create_or_update](const nlohmann::json &request) {
                            return run_create_or_update(request, "update");
                        })
                    .on("get",
                        [&service, &default_agent_key](const nlohmann::json &request) {
                            return builtin::detail::require_named_entity(request, "Error: id or name is required.", [&](const std::string &selector) {
                                const auto automation = service.find(default_agent_key, selector);
                                if (!automation.has_value()) {
                                    return tool_dispatch::response{.message = "Error: automation not found.", .is_error = true};
                                }
                                return tool_dispatch::response{.message = automation_to_json(*automation).dump()};
                            });
                        })
                    .on("list",
                        [&service, ctx](const nlohmann::json &request) {
                            automation::AutomationQuery query;
                            query.agent_key = builtin::detail::resolve_query_agent_key(ctx, request);
                            if (const auto it = request.find("enabled"); it != request.end() && it->is_boolean()) {
                                query.enabled = it->get<bool>();
                            }
                            if (const auto it = request.find("paused"); it != request.end() && it->is_boolean()) {
                                query.paused = it->get<bool>();
                            }

                            auto entries = nlohmann::json::array();
                            for (const auto &automation : service.list(query)) {
                                entries.push_back(automation_to_json(automation));
                            }
                            return tool_dispatch::response{.message = entries.dump()};
                        })
                    .on("remove",
                        [&service, &default_agent_key](const nlohmann::json &request) {
                            return builtin::detail::require_named_entity(request, "Error: id or name is required.", [&](const std::string &selector) {
                                const bool removed = service.remove(default_agent_key, selector);
                                return tool_dispatch::response{.message = removed ? "Removed automation." : "Error: automation not found.", .is_error = !removed};
                            });
                        })
                    .on("run",
                        [&service, &default_agent_key](const nlohmann::json &request) {
                            return builtin::detail::require_named_entity(request, "Error: id or name is required.", [&](const std::string &selector) {
                                try {
                                    const auto run_id = service.run_now(default_agent_key, selector);
                                    return tool_dispatch::response{.message = "Ran automation (" + run_id + ")."};
                                } catch (const std::exception &error) {
                                    return tool_dispatch::response{.message = "Error: " + std::string(error.what()) + ".", .is_error = true};
                                }
                            });
                        })
                    .on("pause",
                        [&service, &default_agent_key](const nlohmann::json &request) {
                            return builtin::detail::require_named_entity(request, "Error: id or name is required.", [&](const std::string &selector) {
                                const bool paused = service.pause(default_agent_key, selector);
                                return tool_dispatch::response{.message = paused ? "Paused automation." : "Error: automation not found.", .is_error = !paused};
                            });
                        })
                    .on("resume",
                        [&service, &default_agent_key](const nlohmann::json &request) {
                            return builtin::detail::require_named_entity(request, "Error: id or name is required.", [&](const std::string &selector) {
                                const bool resumed = service.resume(default_agent_key, selector);
                                return tool_dispatch::response{.message = resumed ? "Resumed automation." : "Error: automation not found.", .is_error = !resumed};
                            });
                        })
                    .on("list_runs",
                        [&service, ctx](const nlohmann::json &request) {
                            const auto query = parse_run_query(request, ctx, service);
                            if (!query.has_value()) {
                                return tool_dispatch::response{.message = "Error: " + query.error() + ".", .is_error = true};
                            }

                            auto runs = nlohmann::json::array();
                            for (const auto &run : service.list_runs(*query)) {
                                runs.push_back(run_to_json(run));
                            }
                            return tool_dispatch::response{.message = runs.dump()};
                        })
                    .on("list_deliveries",
                        [&service, ctx](const nlohmann::json &request) {
                            const auto query = parse_delivery_query(request, ctx, service);
                            if (!query.has_value()) {
                                return tool_dispatch::response{.message = "Error: " + query.error() + ".", .is_error = true};
                            }

                            auto deliveries = nlohmann::json::array();
                            for (const auto &delivery : service.list_deliveries(*query)) {
                                deliveries.push_back(delivery_to_json(delivery));
                            }
                            return tool_dispatch::response{.message = deliveries.dump()};
                        })
                    .on("ack_delivery",
                        [&service, ctx](const nlohmann::json &request) {
                            const auto delivery_id = request.value("delivery_id", request.value("id", ""));
                            if (delivery_id.empty()) {
                                return tool_dispatch::response{.message = "Error: delivery_id is required.", .is_error = true};
                            }

                            const auto agent_key = builtin::detail::resolve_query_agent_key(ctx, request);
                            if (agent_key.empty()) {
                                return tool_dispatch::response{.message = "Error: agent_key is required.", .is_error = true};
                            }

                            const bool acked = service.ack_delivery(agent_key, delivery_id);
                            return tool_dispatch::response{.message = acked ? "Acknowledged delivery." : "Error: delivery not found.", .is_error = !acked};
                        })
                    .on("clear_deliveries",
                        [&service, ctx](const nlohmann::json &request) {
                            const auto query = parse_delivery_query(request, ctx, service);
                            if (!query.has_value()) {
                                return tool_dispatch::response{.message = "Error: " + query.error() + ".", .is_error = true};
                            }
                            if (query->agent_key.empty()) {
                                return tool_dispatch::response{.message = "Error: agent_key is required.", .is_error = true};
                            }

                            try {
                                service.clear_deliveries(*query);
                                return tool_dispatch::response{.message = "Cleared deliveries."};
                            } catch (const std::exception &error) {
                                return tool_dispatch::response{.message = "Error: " + std::string(error.what()) + ".", .is_error = true};
                            }
                        }),
                builtin::detail::normalize_automation_op_input(input));
        }

    } // namespace

    void register_automation_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        contextual_tool_group()
            .require_automation_service()
            .add(make_tool_spec_builder("automation")
                     .description("Manage unified automations for the current agent.")
                     .input_schema(schema_fragments::object_with_required(
                         {
                             {"op", schema_fragments::op_enum({"create", "update", "get", "list", "remove", "run", "pause", "resume", "list_runs",
                                                              "list_deliveries", "ack_delivery", "clear_deliveries"})},
                             {"id", schema_fragments::id_field()},
                             {"name", {{"type", "string"}}},
                             {"delivery_id", {{"type", "string"}}},
                             {"automation_id", {{"type", "string"}}},
                             {"agent_key", {{"type", "string"}}},
                             {"prompt", {{"type", "string"}}},
                             {"notes", {{"type", "string"}}},
                             {"enabled", {{"type", "boolean"}}},
                             {"paused", {{"type", "boolean"}}},
                             {"only_unacked", {{"type", "boolean"}}},
                             {"run_id", {{"type", "string"}}},
                             {"target", {{"type", "string"}}},
                             {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                             {"trigger",
                              schema_fragments::object_with_required(
                                  {
                                      {"type", schema_fragments::op_enum({"cron", "interval", "once"})},
                                      {"cron", {{"type", "string"}}},
                                      {"at", {{"type", "string"}}},
                                      {"every", {{"type", "string"}}},
                                      {"jitter", {{"type", "string"}}},
                                      {"time_zone", {{"type", "string"}}},
                                      {"active_windows", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                                  },
                                  {"type"})},
                             {"delivery",
                              schema_fragments::object_with_required(
                                  {
                                      {"mode", schema_fragments::delivery_mode_field()},
                                      {"targets", schema_fragments::delivery_targets_field()},
                                  },
                                  {})},
                         },
                         {"op"}))
                     .execute([tool_context](const nlohmann::json &input) {
                         return execute_automation_tool(input, tool_context);
                     })
                     .deferred())
            .register_into(registry, tool_context);
    }

} // namespace orangutan::tools
