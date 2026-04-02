#include "channel/qq/qq-channel.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace orangutan;

namespace orangutan::channel::qq {

    struct QqChannelTestAccess {
        static void set_api_client(QqChannel &channel, std::unique_ptr<QqApiClient> api_client) {
            channel.api_client_ = std::move(api_client);
        }

        static void set_on_message(QqChannel &channel, MessageCallback callback) {
            channel.on_message_ = std::move(callback);
        }

        static void remember_inbound_message(QqChannel &channel, const std::string &message_id) {
            channel.remember_inbound_message(message_id);
        }

        static bool consume_passive_reply_quota(QqChannel &channel, const std::string &message_id, int reply_units = 1) {
            return channel.consume_passive_reply_quota(message_id, reply_units);
        }

        static std::string resolve_passive_reply_message_id(QqChannel &channel, const std::string &message_id, int reply_units = 1) {
            return channel.resolve_passive_reply_message_id(message_id, reply_units);
        }

        static void send_text_now(QqChannel &channel, const std::string &jid, const std::string &content, const std::string &reply_to_message_id,
                                  const std::string &reference_message_id) {
            channel.send_outbound_now(jid, OutboundMessage{
                                               .payload = TextPayload{.text = content},
                                               .reply_to_message_id = reply_to_message_id,
                                               .reference_message_id = reference_message_id,
                                           });
        }

        static void handle_c2c_message(QqChannel &channel, const nlohmann::json &data) {
            channel.handle_c2c_message(data);
        }

        static void handle_reaction_event(QqChannel &channel, const std::string &event_type, const nlohmann::json &data) {
            channel.handle_reaction_event(event_type, data);
        }

        static std::string parse_message_scene_ext_value(const nlohmann::json &data, std::string_view key) {
            return QqChannel::parse_message_scene_ext_value(data, key);
        }
    };

} // namespace orangutan::channel::qq

namespace {

    namespace qqtest = orangutan::channel::qq;

    class FakeQqApiClient final : public QqApiClient {
    public:
        struct Request {
            std::string method;
            std::string path;
            nlohmann::json body;
        };

        FakeQqApiClient()
        : QqApiClient("app-id", "client-secret") {}

        [[nodiscard]]
        std::string get_gateway_url() override {
            return "wss://gateway.test";
        }

        [[nodiscard]]
        QqApiResponse get(const std::string &path) override {
            last_get_path = path;
            return download_response;
        }

        [[nodiscard]]
        QqApiResponse post(const std::string &path, const nlohmann::json &body) override {
            requests.push_back({
                .method = "POST",
                .path = path,
                .body = body,
            });
            if (path.ends_with("/files")) {
                return QqApiResponse{
                    .http_status = 200,
                    .body = R"({"file_info":"file-info-uploaded"})",
                };
            }
            return QqApiResponse{
                .http_status = 200,
                .body = "{}",
            };
        }

        [[nodiscard]]
        QqApiResponse put(const std::string &path, const nlohmann::json &body) override {
            requests.push_back({
                .method = "PUT",
                .path = path,
                .body = body,
            });
            return QqApiResponse{
                .http_status = 200,
                .body = "{}",
            };
        }

        [[nodiscard]]
        QqApiResponse del(const std::string &path) override {
            requests.push_back({
                .method = "DELETE",
                .path = path,
                .body = nlohmann::json::object(),
            });
            return QqApiResponse{
                .http_status = 204,
                .body = {},
            };
        }

        std::vector<Request> requests;
        QqApiResponse download_response{
            .http_status = 200,
            .body = "attachment-bytes",
        };
        std::string last_get_path;
    };

    TEST_CASE("qq_channel_passive_reply_quota_requires_known_message_and_enforces_limit") {
        const auto temp_root = testing::unique_test_root("qq-channel");
        const testing::ScopedEnvVar home_var("HOME", temp_root.string());
        QqChannel channel("bot", "app-id", "client-secret");

        CHECK_FALSE(qqtest::QqChannelTestAccess::consume_passive_reply_quota(channel, "unknown"));
        CHECK(qqtest::QqChannelTestAccess::resolve_passive_reply_message_id(channel, "unknown").empty());

        qqtest::QqChannelTestAccess::remember_inbound_message(channel, "message-1");
        CHECK(qqtest::QqChannelTestAccess::consume_passive_reply_quota(channel, "message-1", 2));
        CHECK(qqtest::QqChannelTestAccess::consume_passive_reply_quota(channel, "message-1", 2));
        CHECK_FALSE(qqtest::QqChannelTestAccess::consume_passive_reply_quota(channel, "message-1"));
    }

    TEST_CASE("qq_channel_chunked_reply_falls_back_to_proactive_when_reply_quota_is_insufficient") {
        const auto temp_root = testing::unique_test_root("qq-channel");
        const testing::ScopedEnvVar home_var("HOME", temp_root.string());
        QqChannel channel("bot", "app-id", "client-secret");
        auto fake_api = std::make_unique<FakeQqApiClient>();
        auto *api = fake_api.get();
        qqtest::QqChannelTestAccess::set_api_client(channel, std::move(fake_api));

        qqtest::QqChannelTestAccess::remember_inbound_message(channel, "message-1");
        const std::string long_text(16050, 'a');

        qqtest::QqChannelTestAccess::send_text_now(channel, "qqbot:bot:c2c:user-openid", long_text, "message-1", "message-1");

        REQUIRE(api->requests.size() == 5UL);
        for (const auto &request : api->requests) {
            CHECK(request.path == "/v2/users/user-openid/messages");
            CHECK_FALSE(request.body.contains("msg_id"));
            CHECK_FALSE(request.body.contains("message_reference"));
        }
    }

    TEST_CASE("qq_channel_media_send_supports_caption_text_and_reference") {
        const auto temp_root = testing::unique_test_root("qq-channel");
        const testing::ScopedEnvVar home_var("HOME", temp_root.string());
        QqChannel channel("bot", "app-id", "client-secret");
        auto fake_api = std::make_unique<FakeQqApiClient>();
        auto *api = fake_api.get();
        qqtest::QqChannelTestAccess::set_api_client(channel, std::move(fake_api));
        qqtest::QqChannelTestAccess::remember_inbound_message(channel, "message-1");

        channel.send_media_message("qqbot:bot:c2c:user-openid", 1, "https://example.test/image.png", "message-1", "caption", "message-ref-1");

        REQUIRE(api->requests.size() == 2UL);
        CHECK(api->requests[0].path == "/v2/users/user-openid/files");
        CHECK(api->requests[0].body.at("file_type").get<int>() == 1);
        CHECK(api->requests[0].body.at("url").get<std::string>() == "https://example.test/image.png");
        CHECK_FALSE(api->requests[0].body.at("srv_send_msg").get<bool>());

        CHECK(api->requests[1].path == "/v2/users/user-openid/messages");
        CHECK(api->requests[1].body.at("msg_type").get<int>() == 7);
        CHECK(api->requests[1].body.at("media").at("file_info").get<std::string>() == "file-info-uploaded");
        CHECK(api->requests[1].body.at("content").get<std::string>() == "caption");
        CHECK(api->requests[1].body.at("msg_id").get<std::string>() == "message-1");
        CHECK(api->requests[1].body.at("message_reference").at("message_id").get<std::string>() == "message-ref-1");
    }

    TEST_CASE("qq_channel_inbound_c2c_message_exposes_attachment_metadata_and_extracts_reference_index") {
        const auto temp_root = testing::unique_test_root("qq-channel");
        const testing::ScopedEnvVar home_var("HOME", temp_root.string());

        QqChannel channel("bot", "app-id", "client-secret");
        auto fake_api = std::make_unique<FakeQqApiClient>();
        auto *api = fake_api.get();
        fake_api->download_response = QqApiResponse{
            .http_status = 200,
            .body = "png-bytes",
        };
        qqtest::QqChannelTestAccess::set_api_client(channel, std::move(fake_api));

        InboundMessage captured;
        bool called = false;
        qqtest::QqChannelTestAccess::set_on_message(channel, [&](const InboundMessage &message) {
            captured = message;
            called = true;
        });

        const nlohmann::json data = {
            {"id", "message-attachment-1"},
            {"content", "hello"},
            {"timestamp", "2026-04-02T12:00:00Z"},
            {"author", {{"id", "user-1"}, {"user_openid", "user-openid"}}},
            {"message_scene", {{"ext", nlohmann::json::array({"ref_msg_idx=REFIDX_123"})}}},
            {"attachments", nlohmann::json::array({
                                {
                                    {"content_type", "image/png"},
                                    {"url", "https://attachment.test/image.png"},
                                    {"filename", "image.png"},
                                    {"width", 10},
                                    {"height", 20},
                                    {"size", 42},
                                },
                            })},
        };

        qqtest::QqChannelTestAccess::handle_c2c_message(channel, data);

        REQUIRE(called);
        CHECK(captured.jid == "qqbot:bot:c2c:user-openid");
        CHECK(captured.reference_message_index == "REFIDX_123");
        REQUIRE(captured.attachments.size() == 1UL);
        CHECK(api->last_get_path.empty());
        CHECK(captured.attachments.front().download_pending);
        CHECK(captured.attachments.front().download_error.empty());
        CHECK(captured.attachments.front().local_path.empty());

        const auto destination = temp_root / "workspace" / "image.png";
        const auto downloaded = channel.download_attachment(captured.jid, captured.attachments.front(), destination.string());

        CHECK(api->last_get_path == "https://attachment.test/image.png");
        CHECK_FALSE(downloaded.download_pending);
        CHECK(downloaded.download_error.empty());
        CHECK(downloaded.local_path == destination.string());
        CHECK(std::filesystem::exists(downloaded.local_path));

        std::ifstream input(downloaded.local_path, std::ios::binary);
        std::string file_contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        CHECK(file_contents == "png-bytes");
        CHECK(qqtest::QqChannelTestAccess::parse_message_scene_ext_value(data, "ref_msg_idx") == "REFIDX_123");
    }

    TEST_CASE("qq_channel_reaction_events_are_emitted_as_structured_inbound_events") {
        const auto temp_root = testing::unique_test_root("qq-channel");
        const testing::ScopedEnvVar home_var("HOME", temp_root.string());

        QqChannel channel("bot", "app-id", "client-secret");
        InboundMessage captured;
        bool called = false;
        qqtest::QqChannelTestAccess::set_on_message(channel, [&](const InboundMessage &message) {
            captured = message;
            called = true;
        });

        qqtest::QqChannelTestAccess::handle_reaction_event(channel, "MESSAGE_REACTION_ADD",
                                                           nlohmann::json{
                                                               {"channel_id", "guild-channel-1"},
                                                               {"user_id", "user-1"},
                                                               {"timestamp", "2026-04-02T13:00:00Z"},
                                                               {"target", {{"id", "message-1"}, {"type", 0}}},
                                                               {"emoji", {{"id", "128512"}, {"type", 1}}},
                                                           });

        REQUIRE(called);
        CHECK(captured.event_kind == InboundEventKind::reaction_added);
        CHECK(captured.jid == "qqbot:bot:guild:guild-channel-1");
        REQUIRE(captured.reaction.has_value());
        CHECK(captured.reaction->target_id == "message-1");
        CHECK(captured.reaction->emoji_id == "128512");
        CHECK(captured.reaction->emoji_type == 1);
    }

} // namespace
