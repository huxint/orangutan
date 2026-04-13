#include "channel/allowlist.hpp"
#include "channel/channel.hpp"
#include "channel/message-queue.hpp"

#include <nlohmann/json.hpp>
#include <concepts>
#include <chrono>
#include <filesystem>
#include <future>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace orangutan;

namespace {

    using ChannelTextSendSignature = void (Channel::*)(std::string_view, std::string_view, std::string_view);
    using ChannelMarkdownSendSignature = void (Channel::*)(std::string_view, std::string_view, std::string_view, std::string_view);
    using ChannelMediaSendSignature = void (Channel::*)(std::string_view, int, std::string_view, std::string_view, std::string_view, std::string_view);
    using ManagerTextSendSignature = void (ChannelManager::*)(std::string_view, std::string_view, std::string_view);
    using ManagerMarkdownSendSignature = void (ChannelManager::*)(std::string_view, std::string_view, std::string_view, std::string_view);
    using ManagerMediaSendSignature = void (ChannelManager::*)(std::string_view, int, std::string_view, std::string_view, std::string_view, std::string_view);
    using ChannelDownloadSignature = Attachment (Channel::*)(std::string_view, const Attachment &, const std::filesystem::path &);
    using ManagerDownloadSignature = Attachment (ChannelManager::*)(std::string_view, const Attachment &, const std::filesystem::path &);

    static_assert(std::same_as<decltype(&Channel::send_message), ChannelTextSendSignature>);
    static_assert(std::same_as<decltype(&Channel::send_markdown_message), ChannelMarkdownSendSignature>);
    static_assert(std::same_as<decltype(&Channel::send_media_message), ChannelMediaSendSignature>);
    static_assert(std::same_as<decltype(static_cast<ManagerTextSendSignature>(&ChannelManager::send)), ManagerTextSendSignature>);
    static_assert(std::same_as<decltype(&ChannelManager::send_markdown), ManagerMarkdownSendSignature>);
    static_assert(std::same_as<decltype(&ChannelManager::send_media), ManagerMediaSendSignature>);
    static_assert(std::same_as<decltype(&Channel::download_attachment), ChannelDownloadSignature>);
    static_assert(std::same_as<decltype(&ChannelManager::download_attachment), ManagerDownloadSignature>);

    class MockChannel final : public Channel {
    public:
        struct MediaMessage {
            std::string jid;
            int file_type = 0;
            std::string url;
            std::string reply_to_message_id;
            std::string caption;
        };

        struct KeyboardMessage {
            std::string jid;
            std::string markdown;
            nlohmann::json keyboard_payload;
            std::string reply_to_message_id;
        };

        struct StructuredMessage {
            std::string jid;
            nlohmann::json payload;
            std::string reply_to_message_id;
            std::string reference_message_id;
        };

        struct Reaction {
            std::string jid;
            std::string message_id;
            std::string type;
            std::string id;
        };

        MockChannel(std::string channel_name, std::string jid_prefix)
        : name_(std::move(channel_name)),
          jid_prefix_(std::move(jid_prefix)) {}

        std::string name() const override {
            return name_;
        }

        void connect(MessageCallback on_message) override {
            connected_ = true;
            on_message_ = std::move(on_message);
        }

        void send(const std::string &jid, const OutboundMessage &message) override {
            std::visit(
                [&]<typename T>(const T &payload) {
                    using Payload = std::decay_t<T>;
                    if constexpr (std::same_as<Payload, TextPayload>) {
                        sent_messages_.emplace_back(jid, payload.text);
                    } else if constexpr (std::same_as<Payload, MarkdownPayload>) {
                        sent_markdown_messages_.emplace_back(jid, payload.markdown);
                    } else if constexpr (std::same_as<Payload, MediaPayload>) {
                        sent_media_messages_.push_back({jid, payload.file_type, payload.url, message.reply_to_message_id, payload.caption});
                    } else if constexpr (std::same_as<Payload, KeyboardPayload>) {
                        sent_keyboard_messages_.push_back({jid, payload.markdown, payload.keyboard_payload, message.reply_to_message_id});
                    } else if constexpr (std::same_as<Payload, ArkPayload>) {
                        sent_ark_messages_.push_back({jid, payload.ark_payload, message.reply_to_message_id, message.reference_message_id});
                    } else if constexpr (std::same_as<Payload, EmbedPayload>) {
                        sent_embed_messages_.push_back({jid, payload.embed_payload, message.reply_to_message_id, message.reference_message_id});
                    }
                },
                message.payload);

            sent_reply_to_ids_.push_back(message.reply_to_message_id);
            sent_reference_ids_.push_back(message.reference_message_id);
        }

        void add_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) override {
            added_reactions_.push_back({.jid = jid, .message_id = message_id, .type = type, .id = id});
        }

        void remove_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) override {
            removed_reactions_.push_back({.jid = jid, .message_id = message_id, .type = type, .id = id});
        }

        void disconnect() override {
            connected_ = false;
        }

        bool owns_jid(const std::string &jid) const override {
            return jid.starts_with(jid_prefix_);
        }

        bool is_connected() const override {
            return connected_;
        }

        [[nodiscard]]
        std::vector<std::string> known_user_jids() const override {
            return known_user_jids_;
        }

        void emit(const InboundMessage &msg) const {
            if (!on_message_) {
                throw std::runtime_error("mock channel not connected");
            }
            on_message_(msg);
        }

        [[nodiscard]]
        const std::vector<std::pair<std::string, std::string>> &sent_messages() const {
            return sent_messages_;
        }

        [[nodiscard]]
        const std::vector<std::string> &sent_reply_to_ids() const {
            return sent_reply_to_ids_;
        }

        [[nodiscard]]
        const std::vector<std::string> &sent_reference_ids() const {
            return sent_reference_ids_;
        }

        [[nodiscard]]
        const std::vector<std::pair<std::string, std::string>> &sent_markdown_messages() const {
            return sent_markdown_messages_;
        }

        [[nodiscard]]
        const std::vector<MediaMessage> &sent_media_messages() const {
            return sent_media_messages_;
        }

        [[nodiscard]]
        const std::vector<KeyboardMessage> &sent_keyboard_messages() const {
            return sent_keyboard_messages_;
        }

        [[nodiscard]]
        const std::vector<StructuredMessage> &sent_ark_messages() const {
            return sent_ark_messages_;
        }

        [[nodiscard]]
        const std::vector<StructuredMessage> &sent_embed_messages() const {
            return sent_embed_messages_;
        }

        [[nodiscard]]
        const std::vector<Reaction> &added_reactions() const {
            return added_reactions_;
        }

        [[nodiscard]]
        const std::vector<Reaction> &removed_reactions() const {
            return removed_reactions_;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
        std::vector<std::string> known_user_jids_;

    private:
        std::string name_;
        std::string jid_prefix_;
        MessageCallback on_message_;
        bool connected_ = false;
        std::vector<std::pair<std::string, std::string>> sent_messages_;
        std::vector<std::pair<std::string, std::string>> sent_markdown_messages_;
        std::vector<MediaMessage> sent_media_messages_;
        std::vector<KeyboardMessage> sent_keyboard_messages_;
        std::vector<StructuredMessage> sent_ark_messages_;
        std::vector<StructuredMessage> sent_embed_messages_;
        std::vector<Reaction> added_reactions_;
        std::vector<Reaction> removed_reactions_;
        std::vector<std::string> sent_reply_to_ids_;
        std::vector<std::string> sent_reference_ids_;
    };

    TEST_CASE("empty_allowlist_allows_any_jid") {
        const Allowlist allowlist({}, {});

        CHECK(allowlist.is_allowed("cli:local"));
        CHECK(allowlist.is_allowed("qqbot:c2c:ABC123"));
        CHECK(allowlist.is_allowed("qqbot:group:XYZ789"));
    };

    TEST_CASE("allow_patterns_support_terminal_wildcard") {
        const Allowlist allowlist({"cli:*", "qqbot:c2c:*"}, {});

        CHECK(allowlist.is_allowed("cli:local"));
        CHECK(allowlist.is_allowed("qqbot:c2c:ABC123"));
        CHECK_FALSE(allowlist.is_allowed("qqbot:group:ABC123"));
    };

    TEST_CASE("deny_patterns_override_allow_rules") {
        const Allowlist allowlist({"qqbot:c2c:*"}, {"qqbot:c2c:BLOCKED"});

        CHECK(allowlist.is_allowed("qqbot:c2c:ALLOWED"));
        CHECK_FALSE(allowlist.is_allowed("qqbot:c2c:BLOCKED"));
    };

    TEST_CASE("try_pop_returns_queued_message") {
        MessageQueue queue;
        queue.push({
            .jid = "qqbot:c2c:123",
            .sender = "123",
            .sender_name = "Alice",
            .content = "hello",
            .timestamp = "2026-03-12T12:00:00Z",
            .is_group = false,
        });

        InboundMessage message;
        REQUIRE(queue.try_pop(message, std::chrono::milliseconds(10)));
        CHECK(message.jid == "qqbot:c2c:123");
        CHECK(message.content == "hello");
    };

    TEST_CASE("try_pop_times_out_when_queue_is_empty") {
        MessageQueue queue;
        InboundMessage message;

        CHECK_FALSE(queue.try_pop(message, std::chrono::milliseconds(20)));
    };

    TEST_CASE("shutdown_unblocks_waiting_try_pop") {
        MessageQueue queue;
        auto future = std::async(std::launch::async, [&queue] {
            InboundMessage message;
            return queue.try_pop(message, std::chrono::seconds(5));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        queue.shutdown();

        CHECK_FALSE(future.get());
    };

    TEST_CASE("connect_routes_inbound_messages_and_outbound_replies") {
        ChannelManager manager;

        auto cli_channel = std::make_unique<MockChannel>("cli", "cli:");
        auto *cli = cli_channel.get();
        manager.add_channel(std::move(cli_channel));

        auto qq_channel = std::make_unique<MockChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        std::vector<InboundMessage> received;
        manager.connect_all([&received](const InboundMessage &msg) {
            received.push_back(msg);
        });

        REQUIRE(manager.has_channels());
        CHECK(cli->is_connected());
        CHECK(qq->is_connected());

        cli->emit({
            .jid = "cli:local",
            .sender = "local",
            .sender_name = "user",
            .content = "ping",
            .timestamp = "2026-03-12T12:00:00Z",
            .is_group = false,
        });
        qq->emit({
            .jid = "qqbot:c2c:456",
            .sender = "456",
            .sender_name = "Bob",
            .content = "pong",
            .timestamp = "2026-03-12T12:00:01Z",
            .is_group = false,
        });

        CHECK(received.size() == 2UL);
        CHECK(received[0].jid == "cli:local");
        CHECK(received[1].jid == "qqbot:c2c:456");

        manager.send("cli:local", "reply to cli");
        manager.send("qqbot:c2c:456", "reply to qq", "message-456");

        CHECK(cli->sent_messages().size() == 1UL);
        CHECK(cli->sent_messages()[0].first == "cli:local");
        CHECK(cli->sent_messages()[0].second == "reply to cli");

        CHECK(qq->sent_messages().size() == 1UL);
        CHECK(qq->sent_messages()[0].first == "qqbot:c2c:456");
        CHECK(qq->sent_messages()[0].second == "reply to qq");
        CHECK(qq->sent_reply_to_ids().size() == 1UL);
        CHECK(qq->sent_reply_to_ids()[0] == "message-456");

        manager.disconnect_all();
        CHECK_FALSE(cli->is_connected());
        CHECK_FALSE(qq->is_connected());
    };

    TEST_CASE("channel_manager_routes_structured_messages_reactions_and_known_users") {
        ChannelManager manager;

        auto qq_channel = std::make_unique<MockChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        qq->known_user_jids_ = {"qqbot:guild:1", "qqbot:c2c:2"};
        manager.add_channel(std::move(qq_channel));

        const auto keyboard = nlohmann::json{
            {"content", {{"rows", nlohmann::json::array()}}},
        };
        const auto ark = nlohmann::json{{"template_id", 23}};
        const auto embed = nlohmann::json{{"title", "hello"}};

        manager.send_markdown("qqbot:guild:1", "# title", "msg-1", "msg-ref-1");
        manager.send_media("qqbot:guild:1", 1, "https://example.test/image.png", "msg-2", "cover", "msg-ref-2");
        manager.send_keyboard("qqbot:guild:1", "pick", keyboard, "msg-3", "msg-ref-3");
        manager.send_ark("qqbot:guild:1", ark, "msg-4", "msg-ref-4");
        manager.send_embed("qqbot:guild:1", embed, "msg-5", "msg-ref-5");
        manager.add_reaction("qqbot:guild:1", "message-1", "emoji", "128512");
        manager.remove_reaction("qqbot:guild:1", "message-1", "emoji", "128512");

        REQUIRE(qq->sent_markdown_messages().size() == 1UL);
        CHECK(qq->sent_markdown_messages().front().second == "# title");

        REQUIRE(qq->sent_media_messages().size() == 1UL);
        CHECK(qq->sent_media_messages().front().file_type == 1);
        CHECK(qq->sent_media_messages().front().url == "https://example.test/image.png");
        CHECK(qq->sent_media_messages().front().caption == "cover");

        REQUIRE(qq->sent_keyboard_messages().size() == 1UL);
        CHECK(qq->sent_keyboard_messages().front().keyboard_payload == keyboard);

        REQUIRE(qq->sent_ark_messages().size() == 1UL);
        CHECK(qq->sent_ark_messages().front().payload == ark);

        REQUIRE(qq->sent_embed_messages().size() == 1UL);
        CHECK(qq->sent_embed_messages().front().payload == embed);

        REQUIRE(qq->added_reactions().size() == 1UL);
        CHECK(qq->added_reactions().front().message_id == "message-1");
        REQUIRE(qq->removed_reactions().size() == 1UL);
        CHECK(qq->removed_reactions().front().id == "128512");

        REQUIRE(qq->sent_reference_ids().size() == 5UL);
        CHECK(qq->sent_reference_ids()[0] == "msg-ref-1");
        CHECK(qq->sent_reference_ids()[1] == "msg-ref-2");
        CHECK(qq->sent_reference_ids()[2] == "msg-ref-3");
        CHECK(qq->sent_reference_ids()[3] == "msg-ref-4");
        CHECK(qq->sent_reference_ids()[4] == "msg-ref-5");

        const auto known_users = manager.known_user_jids();
        REQUIRE(known_users.size() == 2UL);
        CHECK(known_users[0] == "qqbot:c2c:2");
        CHECK(known_users[1] == "qqbot:guild:1");
    };

    TEST_CASE("channel_manager_convenience_apis_accept_views_and_paths") {
        ChannelManager manager;

        auto cli_channel = std::make_unique<MockChannel>("cli", "cli:");
        auto *cli = cli_channel.get();
        manager.add_channel(std::move(cli_channel));

        const auto destination = std::filesystem::path{"downloads/out.txt"};
        Attachment attachment{
            .filename = "out.txt",
        };

        CHECK_NOTHROW(manager.send(std::string_view{"cli:local"}, std::string_view{"hello"}, std::string_view{"reply-1"}));
        CHECK_NOTHROW(manager.send_markdown(std::string_view{"cli:local"}, std::string_view{"# heading"}, std::string_view{"reply-2"}, std::string_view{"ref-2"}));
        CHECK_NOTHROW(manager.send_media(std::string_view{"cli:local"}, 1, std::string_view{"https://example.test/a.png"}, std::string_view{"reply-3"}, std::string_view{"caption"},
                                         std::string_view{"ref-3"}));
        CHECK_THROWS(manager.download_attachment(std::string_view{"cli:local"}, attachment, destination));

        REQUIRE(cli->sent_messages().size() == 1UL);
        CHECK(cli->sent_messages().front().second == "hello");
        REQUIRE(cli->sent_markdown_messages().size() == 1UL);
        CHECK(cli->sent_markdown_messages().front().second == "# heading");
        REQUIRE(cli->sent_media_messages().size() == 1UL);
        CHECK(cli->sent_media_messages().front().caption == "caption");
    };

} // namespace
