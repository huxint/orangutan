#include "tools/message-attachments/message-attachments-tool.hpp"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "tools/internal.hpp"
#include "tools/registry/contextual-tool-group.hpp"
#include "tools/registry/schema-fragments.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-dispatch.hpp"
#include "tools/registry/tool-spec-builder.hpp"

namespace orangutan::tools {

    namespace {

        std::string default_attachment_name(const Attachment &attachment, std::size_t index) {
            if (!attachment.filename.empty()) {
                return attachment.filename;
            }
            return "attachment-" + std::to_string(index);
        }

        nlohmann::json describe_attachment(const Attachment &attachment, std::size_t index) {
            return nlohmann::json{
                {"index", index},
                {"filename", attachment.filename},
                {"content_type", attachment.content_type},
                {"url", attachment.url},
                {"width", attachment.width},
                {"height", attachment.height},
                {"size", attachment.size},
                {"download_pending", attachment.download_pending},
                {"local_path", attachment.local_path},
                {"download_error", attachment.download_error},
            };
        }

        std::string execute_message_attachments_tool(const nlohmann::json &input, const std::filesystem::path &workspace_root, const ToolRuntimeContext *tool_context) {
            if (tool_context == nullptr) {
                return R"({"error":"message_attachments tool is not available in this context."})";
            }

            const auto &attachments = tool_context->current_message_attachments;
            if (attachments.empty()) {
                return R"({"error":"the current message has no attachments."})";
            }

            const auto normalized_op = input.value("op", "list");
            auto routed_input = input;
            routed_input["op"] = normalized_op;

            const auto result = tool_dispatch()
                                    .unknown_op_error_formatter([](std::string_view) {
                                        return R"({"error":"unknown operation. Supported: list, download."})";
                                    })
                                    .on("list",
                                        [&attachments](const nlohmann::json &) {
                                            nlohmann::json payload = nlohmann::json::array();
                                            for (std::size_t index = 0; index < attachments.size(); ++index) {
                                                payload.push_back(describe_attachment(attachments.at(index), index));
                                            }
                                            return tool_dispatch::response{payload.dump(2)};
                                        })
                                    .on("download",
                                        [&attachments, &workspace_root, &tool_context](const nlohmann::json &request) {
                                            if (!tool_context->attachment_download_callback) {
                                                return tool_dispatch::response{R"({"error":"attachment downloads are not available in this context."})", true};
                                            }

                                            const auto requested_index = request.value("index", -1);
                                            if (requested_index < 0 || static_cast<std::size_t>(requested_index) >= attachments.size()) {
                                                return tool_dispatch::response{R"({"error":"index is required and must refer to an attachment from the current message."})", true};
                                            }

                                            const auto attachment_index = static_cast<std::size_t>(requested_index);
                                            const auto &attachment = attachments.at(attachment_index);
                                            const auto requested_target = request.value("target_path", default_attachment_name(attachment, attachment_index));
                                            const auto resolved_target = resolve_tool_path(std::filesystem::path(requested_target), workspace_root);
                                            const auto downloaded = tool_context->attachment_download_callback(attachment, resolved_target.string());

                                            return tool_dispatch::response{nlohmann::json{{"saved_to", downloaded.local_path},
                                                                                          {"download_error", downloaded.download_error},
                                                                                          {"attachment", describe_attachment(downloaded, attachment_index)}}
                                                                               .dump(2)};
                                        })
                                    .run(routed_input);

            return result.message;
        }

    } // namespace

    void register_message_attachments_tool(ToolRegistry &registry, const std::string &workspace, const ToolRuntimeContext *tool_context) {
        const auto workspace_root = workspace.empty() ? std::filesystem::path{} : std::filesystem::path(workspace);

        contextual_tool_group()
            .require_channel_origin(base::origin::channel)
            .add(tool_spec_builder("message_attachments")
                     .description("Inspect attachment metadata from the current inbound channel message and download one into the workspace only on explicit demand.")
                     .input_schema(schema_fragments::object_with_required(
                         {
                             {"op", schema_fragments::op_enum({"list", "download"})},
                             {"index", {{"type", "integer"}, {"minimum", 0}}},
                             {"target_path", {{"type", "string"}}},
                         },
                         {"op"}))
                     .execute([workspace_root, tool_context](const nlohmann::json &input) {
                         return execute_message_attachments_tool(input, workspace_root, tool_context);
                     })
                     .deferred())
            .register_into(registry, tool_context);
    }

} // namespace orangutan::tools
