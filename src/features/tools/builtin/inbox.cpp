#include "features/tools/builtin/inbox.hpp"

#include "features/automation/runtime.hpp"

#include "infra/format.hpp"

namespace orangutan {
    namespace {

        std::string execute_inbox_tool(const json &input, const ToolRuntimeContext *ctx) {
            if (ctx == nullptr || ctx->automation_runtime == nullptr) {
                return "Error: inbox tool is not available in this context.";
            }

            const auto op = input.value("op", "");
            const auto agent_key = ctx->agent_key.empty() ? std::string("default") : ctx->agent_key;
            auto &runtime = *ctx->automation_runtime;

            if (op == "list") {
                const auto items = runtime.list_inbox(agent_key);
                if (items.empty()) {
                    return "Inbox is empty.";
                }
                std::string out;
                for (const auto &item : items) {
                    append(out, "- {} [{}] {}\n", item.id, item.status, item.title);
                    out.append("  ");
                    out.append(item.body);
                    out.push_back('\n');
                }
                return out;
            }

            if (op == "clear") {
                runtime.clear_inbox(agent_key);
                return "Inbox cleared.";
            }

            const auto id = input.value("id", "");
            if (id.empty()) {
                return "Error: id is required.";
            }

            if (op == "ack") {
                return runtime.ack_inbox(agent_key, id) ? "Inbox item acknowledged." : "Error: inbox item not found.";
            }

            return "Error: unknown operation. Supported: list, ack, clear.";
        }

    } // namespace

    void register_inbox_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr || tool_context->automation_runtime == nullptr) {
            return;
        }

        registry.register_tool({
            .definition =
                {
                    .name = "inbox",
                    .description = "Inspect and manage unread automation delivery results for the current agent.",
                    .input_schema =
                        {
                            {"type", "object"},
                            {"properties",
                             {
                                 {"op", {{"type", "string"}, {"enum", json::array({"list", "ack", "clear"})}}},
                                 {"id", {{"type", "string"}}},
                             }},
                            {"required", json::array({"op"})},
                        },
                },
            .execute =
                [tool_context](const json &input) {
                    return execute_inbox_tool(input, tool_context);
                },
        });
    }

} // namespace orangutan
