#include "tools/message-attachments/message-attachments-tool.hpp"

#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <vector>

using namespace orangutan;

namespace {

    using RegisterAttachmentsWithContext = void (*)(ToolRegistry &, const std::filesystem::path &, const ToolRuntimeContext *);
    using RegisterAttachmentsWithCapability = void (*)(ToolRegistry &, const std::filesystem::path &, tools::AttachmentCapability);

    static_assert(std::same_as<decltype(static_cast<RegisterAttachmentsWithContext>(&tools::register_message_attachments_tool)), RegisterAttachmentsWithContext>);
    static_assert(std::same_as<decltype(static_cast<RegisterAttachmentsWithCapability>(&tools::register_message_attachments_tool)), RegisterAttachmentsWithCapability>);

    TEST_CASE("message_attachments_list_returns_current_message_metadata_without_downloading") {
        ToolRegistry registry;
        std::vector<Attachment> attachments = {
            Attachment{
                .content_type = "image/png",
                .url = "https://example.test/image.png",
                .filename = "image.png",
                .width = 10,
                .height = 20,
                .size = 42,
                .download_pending = true,
            },
        };
        bool download_called = false;
        ToolRuntimeContext context{
            .runtime_origin = base::origin::channel,
            .raw_caller_id = "qqbot:bot:c2c:user-openid",
            .current_message_attachments = attachments,
            .attachment_download_callback =
                [&download_called](const Attachment &attachment, const std::string &) {
                    download_called = true;
                    return attachment;
                },
        };

        tools::register_message_attachments_tool(registry, testing::test_tmp_root(), &context);
        const auto result = registry.execute(ToolUse("list-attachments", "message_attachments", {{"op", "list"}}));

        REQUIRE_FALSE(result.is_error);
        const auto payload = nlohmann::json::parse(result.content);
        REQUIRE(payload.is_array());
        REQUIRE(payload.size() == 1UL);
        CHECK(payload.at(0).at("filename").get<std::string>() == "image.png");
        CHECK(payload.at(0).at("download_pending").get<bool>());
        CHECK_FALSE(download_called);
    }

    TEST_CASE("message_attachments_defaults_to_list_when_op_is_omitted") {
        ToolRegistry registry;
        std::vector<Attachment> attachments = {
            Attachment{
                .content_type = "image/png",
                .url = "https://example.test/image.png",
                .filename = "image.png",
                .download_pending = true,
            },
        };
        bool download_called = false;
        ToolRuntimeContext context{
            .runtime_origin = base::origin::channel,
            .raw_caller_id = "qqbot:bot:c2c:user-openid",
            .current_message_attachments = attachments,
            .attachment_download_callback =
                [&download_called](const Attachment &attachment, const std::string &) {
                    download_called = true;
                    return attachment;
                },
        };

        tools::register_message_attachments_tool(registry, testing::test_tmp_root(), &context);
        const auto result = registry.execute(ToolUse("default-list-attachments", "message_attachments", nlohmann::json::object()));

        REQUIRE_FALSE(result.is_error);
        const auto payload = nlohmann::json::parse(result.content);
        REQUIRE(payload.is_array());
        REQUIRE(payload.size() == 1UL);
        CHECK(payload.at(0).at("filename").get<std::string>() == "image.png");
        CHECK_FALSE(download_called);
    }

    TEST_CASE("message_attachments_download_uses_runtime_callback_and_workspace_relative_target") {
        const auto workspace = testing::unique_test_root("message-attachments-tool");
        ToolRegistry registry;
        std::vector<Attachment> attachments = {
            Attachment{
                .content_type = "image/png",
                .url = "https://example.test/image.png",
                .filename = "image.png",
                .download_pending = true,
            },
        };
        std::string captured_destination;
        ToolRuntimeContext context{
            .runtime_origin = base::origin::channel,
            .raw_caller_id = "qqbot:bot:c2c:user-openid",
            .current_message_attachments = attachments,
            .attachment_download_callback =
                [&captured_destination](const Attachment &attachment, const std::string &destination_path) {
                    captured_destination = destination_path;
                    auto downloaded = attachment;
                    downloaded.download_pending = false;
                    downloaded.local_path = destination_path;
                    return downloaded;
                },
        };

        tools::register_message_attachments_tool(registry, workspace, &context);
        const auto result = registry.execute(ToolUse("download-attachment", "message_attachments", {{"op", "download"}, {"index", 0}, {"target_path", "incoming/image.png"}}));

        REQUIRE_FALSE(result.is_error);
        const auto payload = nlohmann::json::parse(result.content);
        CHECK(captured_destination == (workspace / "incoming" / "image.png").string());
        CHECK(payload.at("saved_to").get<std::string>() == captured_destination);
        CHECK(payload.at("attachment").at("download_pending").get<bool>() == false);
    }

    TEST_CASE("message_attachments_context_overload_reads_download_callback_at_execute_time") {
        const auto workspace = testing::unique_test_root("message-attachments-tool-live-callback");
        ToolRegistry registry;
        std::vector<Attachment> attachments = {
            Attachment{
                .content_type = "image/png",
                .url = "https://example.test/image.png",
                .filename = "image.png",
                .download_pending = true,
            },
        };
        std::string captured_destination;
        ToolRuntimeContext context{
            .runtime_origin = base::origin::channel,
            .current_message_attachments = attachments,
        };

        tools::register_message_attachments_tool(registry, workspace, &context);
        context.attachment_download_callback = [&captured_destination](const Attachment &attachment, const std::string &destination_path) {
            captured_destination = destination_path;
            auto downloaded = attachment;
            downloaded.download_pending = false;
            downloaded.local_path = destination_path;
            return downloaded;
        };
        const auto result = registry.execute(ToolUse("download-live-callback", "message_attachments", { {"op", "download"}, {"index", 0} }));

        REQUIRE_FALSE(result.is_error);
        const auto payload = nlohmann::json::parse(result.content);
        CHECK(captured_destination == (workspace / "image.png").string());
        CHECK(payload.at("saved_to").get<std::string>() == captured_destination);

        std::filesystem::remove_all(workspace);
    }

    TEST_CASE("message_attachments_download_rejects_target_path_outside_workspace") {
        const auto root = testing::unique_test_root("message-attachments-tool-outside");
        const auto workspace = root / "workspace";
        const auto outside = root / "outside";
        std::filesystem::create_directories(workspace);
        std::filesystem::create_directories(outside);

        ToolRegistry registry;
        std::vector<Attachment> attachments = {
            Attachment{
                .content_type = "image/png",
                .url = "https://example.test/image.png",
                .filename = "image.png",
                .download_pending = true,
            },
        };
        bool download_called = false;
        ToolRuntimeContext context{
            .runtime_origin = base::origin::channel,
            .raw_caller_id = "qqbot:bot:c2c:user-openid",
            .current_message_attachments = attachments,
            .attachment_download_callback =
                [&download_called](const Attachment &attachment, const std::string &) {
                    download_called = true;
                    return attachment;
                },
        };

        tools::register_message_attachments_tool(registry, workspace, &context);
        const auto result = registry.execute(ToolUse("download-attachment-outside", "message_attachments",
                                                     {
                                                         {"op", "download"},
                                                         {"index", 0},
                                                         {"target_path", "../outside/image.png"},
                                                     }));

        CHECK(result.is_error);
        CHECK(result.content.contains("workspace sandbox"));
        CHECK_FALSE(download_called);

        std::filesystem::remove_all(root);
    }

    TEST_CASE("message_attachments_not_registered_for_non_channel_context") {
        ToolRegistry registry;
        ToolRuntimeContext context{
            .runtime_origin = base::origin::cli,
        };

        tools::register_message_attachments_tool(registry, testing::test_tmp_root(), &context);

        CHECK(registry.find_definition("message_attachments") == nullptr);
    }

    TEST_CASE("message_attachments_not_registered_for_non_channel_capability") {
        ToolRegistry registry;
        std::vector<Attachment> attachments = {
            Attachment{
                .content_type = "image/png",
                .url = "https://example.test/image.png",
                .filename = "image.png",
                .download_pending = true,
            },
        };

        tools::register_message_attachments_tool(registry,
                                                 testing::test_tmp_root(),
                                                 tools::AttachmentCapability{
                                                     .runtime_origin = base::origin::cli,
                                                     .current_message_attachments = &attachments,
                                                 });

        CHECK(registry.find_definition("message_attachments") == nullptr);
    }

    TEST_CASE("message_attachments_not_registered_for_null_attachment_capability") {
        ToolRegistry registry;

        tools::register_message_attachments_tool(registry,
                                                 testing::test_tmp_root(),
                                                 tools::AttachmentCapability{
                                                     .runtime_origin = base::origin::channel,
                                                     .current_message_attachments = nullptr,
                                                 });

        CHECK(registry.find_definition("message_attachments") == nullptr);
    }

    TEST_CASE("message_attachments_not_registered_when_context_is_null") {
        ToolRegistry registry;

        tools::register_message_attachments_tool(registry, testing::test_tmp_root(), nullptr);

        CHECK(registry.find_definition("message_attachments") == nullptr);
    }

    TEST_CASE("message_attachments_tool_definition_matches_exact_schema") {
        ToolRegistry registry;
        ToolRuntimeContext context{
            .runtime_origin = base::origin::channel,
            .current_message_attachments =
                {
                    Attachment{
                        .content_type = "image/png",
                        .url = "https://example.test/image.png",
                        .filename = "image.png",
                        .download_pending = true,
                    },
                },
        };

        tools::register_message_attachments_tool(registry, testing::test_tmp_root(), &context);

        const auto *definition = registry.find_definition("message_attachments");
        REQUIRE(definition != nullptr);
        CHECK(definition->name == "message_attachments");
        CHECK(definition->description == "Inspect attachment metadata from the current inbound channel message and download one into the workspace only on explicit demand.");
        CHECK(definition->input_schema == nlohmann::json{
                                              {"type", "object"},
                                              {"properties",
                                               {
                                                   {"op", {{"type", "string"}, {"enum", nlohmann::json::array({"list", "download"})}}},
                                                   {"index", {{"type", "integer"}, {"minimum", 0}}},
                                                   {"target_path", {{"type", "string"}}},
                                               }},
                                              {"required", nlohmann::json::array({"op"})},
                                          });
    }

    TEST_CASE("message_attachments_unknown_op_returns_exact_error") {
        ToolRegistry registry;
        std::vector<Attachment> attachments = {
            Attachment{
                .content_type = "image/png",
                .url = "https://example.test/image.png",
                .filename = "image.png",
                .download_pending = true,
            },
        };
        ToolRuntimeContext context{
            .runtime_origin = base::origin::channel,
            .current_message_attachments = attachments,
            .attachment_download_callback =
                [](const Attachment &attachment, const std::string &) {
                    return attachment;
                },
        };

        tools::register_message_attachments_tool(registry, testing::test_tmp_root(), &context);
        const auto result = registry.execute(ToolUse("unknown-op", "message_attachments", {{"op", "noop"}}));

        CHECK_FALSE(result.is_error);
        const auto payload = nlohmann::json::parse(result.content);
        CHECK(payload.at("error").get<std::string>() == "unknown operation. Supported: list, download.");
    }

    TEST_CASE("message_attachments_list_without_attachments_returns_exact_error") {
        ToolRegistry registry;
        ToolRuntimeContext context{
            .runtime_origin = base::origin::channel,
        };

        tools::register_message_attachments_tool(registry, testing::test_tmp_root(), &context);
        const auto result = registry.execute(ToolUse("list-empty", "message_attachments", {{"op", "list"}}));

        CHECK_FALSE(result.is_error);
        const auto payload = nlohmann::json::parse(result.content);
        CHECK(payload.at("error").get<std::string>() == "the current message has no attachments.");
    }

    TEST_CASE("message_attachments_download_without_callback_returns_exact_error") {
        ToolRegistry registry;
        std::vector<Attachment> attachments = {
            Attachment{
                .content_type = "image/png",
                .url = "https://example.test/image.png",
                .filename = "image.png",
                .download_pending = true,
            },
        };
        ToolRuntimeContext context{
            .runtime_origin = base::origin::channel,
            .current_message_attachments = attachments,
        };

        tools::register_message_attachments_tool(registry, testing::test_tmp_root(), &context);
        const auto result = registry.execute(ToolUse("download-no-callback", "message_attachments", {{"op", "download"}, {"index", 0}}));

        CHECK_FALSE(result.is_error);
        const auto payload = nlohmann::json::parse(result.content);
        CHECK(payload.at("error").get<std::string>() == "attachment downloads are not available in this context.");
    }

} // namespace
