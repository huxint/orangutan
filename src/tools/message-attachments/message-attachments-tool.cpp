#include "tools/message-attachments/message-attachments-tool.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "tools/internal.hpp"
#include "tools/registry/op-tool-support.hpp"
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

        std::string execute_message_attachments_tool(const nlohmann::json &input,
                                                     const std::filesystem::path &workspace_root,
                                                     const std::vector<Attachment> *current_message_attachments,
                                                     const AttachmentDownloadCallback &download_callback) {
            if (current_message_attachments == nullptr) {
                return R"({"error":"message_attachments tool is not available in this context."})";
            }

            const auto &attachments = *current_message_attachments;
            if (attachments.empty()) {
                return R"({"error":"the current message has no attachments."})";
            }

            return dispatch_message(tool_dispatch()
                                        .unknown_op_error_formatter([](std::string_view) {
                                            return R"({"error":"unknown operation. Supported: list, download."})";
                                        })
                                        .on("list",
                                            [&attachments](const nlohmann::json &) {
                                                nlohmann::json payload = nlohmann::json::array();
                                                for (std::size_t index = 0; index < attachments.size(); ++index) {
                                                    payload.push_back(describe_attachment(attachments.at(index), index));
                                                }
                                                return tool_dispatch::response{.message = payload.dump(2)};
                                            })
                                         .on("download",
                                             [&attachments, &workspace_root, &download_callback](const nlohmann::json &request) {
                                                 if (download_callback == nullptr) {
                                                     return tool_dispatch::response{.message = R"({"error":"attachment downloads are not available in this context."})",
                                                                                    .is_error = true};
                                                 }

                                                const auto requested_index = request.value("index", -1);
                                                if (requested_index < 0 || static_cast<std::size_t>(requested_index) >= attachments.size()) {
                                                    return tool_dispatch::response{
                                                        .message = R"({"error":"index is required and must refer to an attachment from the current message."})", .is_error = true};
                                                }

                                                const auto attachment_index = static_cast<std::size_t>(requested_index);
                                                const auto &attachment = attachments.at(attachment_index);
                                                 const auto requested_target = request.value("target_path", default_attachment_name(attachment, attachment_index));
                                                 const auto resolved_target = resolve_tool_path(std::filesystem::path(requested_target), workspace_root);
                                                 const auto downloaded = download_callback(attachment, resolved_target.string());

                                                return tool_dispatch::response{.message = nlohmann::json{{"saved_to", downloaded.local_path},
                                                                                                         {"download_error", downloaded.download_error},
                                                                                                         {"attachment", describe_attachment(downloaded, attachment_index)}}
                                                                                              .dump(2)};
                                            }),
                                     routed_input_with_default_op(input, "list"));
        }

        using MessageAttachmentsExecutor = std::function<std::string(const nlohmann::json &input)>;

        void register_message_attachments_tool_with_executor(ToolRegistry &registry, const std::filesystem::path &workspace_root, MessageAttachmentsExecutor executor) {
            if (auto tool = make_tool_spec_builder("message_attachments")
                                .description("Inspect attachment metadata from the current inbound channel message and download one into the workspace only on explicit demand.")
                                .input_schema(schema_fragments::object_with_required(
                                    {
                                        {"op", schema_fragments::op_enum({"list", "download"})},
                                        {"index", schema_fragments::non_negative_index_field()},
                                        {"target_path", {{"type", "string"}}},
                                    },
                                    {"op"}))
                                .execute(std::move(executor))
                                .deferred()
                                .build();
                tool.has_value()) {
                registry.register_tool(std::move(*tool));
            } else {
                spdlog::warn("failed to register tool: {}", tool.error());
            }
        }

    } // namespace

    void register_message_attachments_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolRuntimeContext *tool_context) {
        if (tool_context == nullptr || tool_context->runtime_origin != base::origin::channel) {
            return;
        }
        register_message_attachments_tool_with_executor(registry, workspace_root, [workspace_root, tool_context](const nlohmann::json &input) {
            return execute_message_attachments_tool(input, workspace_root, &tool_context->current_message_attachments, tool_context->attachment_download_callback);
        });
    }

    void register_message_attachments_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, AttachmentCapability capability) {
        if (capability.runtime_origin != base::origin::channel || capability.current_message_attachments == nullptr) {
            return;
        }

        const auto *current_message_attachments = capability.current_message_attachments;
        auto download_callback = std::move(capability.attachment_download_callback);
        register_message_attachments_tool_with_executor(registry, workspace_root,
                                                        [workspace_root, current_message_attachments, download_callback = std::move(download_callback)](const nlohmann::json &input) {
                                                            return execute_message_attachments_tool(input, workspace_root, current_message_attachments, download_callback);
                                                        });
    }

} // namespace orangutan::tools
