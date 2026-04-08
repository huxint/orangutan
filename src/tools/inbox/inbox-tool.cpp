#include "tools/inbox/inbox-tool.hpp"

#include "automation/scheduler.hpp"

#include "tools/registry/contextual-tool-group.hpp"
#include "tools/registry/schema-fragments.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-dispatch.hpp"
#include "tools/registry/tool-spec-builder.hpp"
#include "utils/format.hpp"

namespace orangutan::tools {
    namespace {

        std::string execute_inbox_tool(const nlohmann::json &input, const ToolRuntimeContext *ctx) {
            if (ctx == nullptr || ctx->automation_runtime == nullptr) {
                return "Error: inbox tool is not available in this context.";
            }

            const auto agent_key = ctx->agent_key.empty() ? std::string("default") : ctx->agent_key;
            auto &runtime = *ctx->automation_runtime;

            const auto normalized_op = input.value("op", "");
            auto routed_input = input;
            routed_input["op"] = normalized_op;

            const auto result = tool_dispatch()
                                    .unknown_op_error("Error: unknown operation. Supported: list, ack, clear.")
                                    .on("list",
                                        [&runtime, &agent_key](const nlohmann::json &) {
                                            const auto items = runtime.list_inbox(agent_key);
                                            if (items.empty()) {
                                                return tool_dispatch::response{.message = "Inbox is empty."};
                                            }
                                            std::string out;
                                            for (const auto &item : items) {
                                                utils::format_to(out, "- {} [{}] {}\n", item.id, item.status, item.title);
                                                out.append("  ");
                                                out.append(item.body);
                                                out.push_back('\n');
                                            }
                                            return tool_dispatch::response{.message = std::move(out)};
                                        })
                                    .on("clear",
                                        [&runtime, &agent_key](const nlohmann::json &) {
                                            runtime.clear_inbox(agent_key);
                                            return tool_dispatch::response{.message = "Inbox cleared."};
                                        })
                                    .on("ack",
                                        [&runtime, &agent_key](const nlohmann::json &request) {
                                            const auto id = request.value("id", "");
                                            if (id.empty()) {
                                                return tool_dispatch::response{.message = "Error: id is required.", .is_error = true};
                                            }
                                            return tool_dispatch::response{.message = runtime.ack_inbox(agent_key, id) ? "Inbox item acknowledged." : "Error: inbox item not found."};
                                        })
                                    .run(routed_input);

            return result.message;
        }

    } // namespace

    void register_inbox_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        contextual_tool_group()
            .require_automation_runtime()
            .add(make_tool_spec_builder("inbox")
                     .description("Inspect and manage unread automation delivery results for the current agent.")
                     .input_schema(schema_fragments::object_with_required(
                         {
                             {"op", schema_fragments::op_enum({"list", "ack", "clear"})},
                             {"id", schema_fragments::id_field()},
                         },
                         {"op"}))
                     .execute([tool_context](const nlohmann::json &input) {
                         return execute_inbox_tool(input, tool_context);
                     })
                     .deferred())
            .register_into(registry, tool_context);
    }

} // namespace orangutan::tools
