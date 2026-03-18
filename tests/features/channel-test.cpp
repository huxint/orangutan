#include "features/channel/core/allowlist.hpp"
#include "features/channel/core/channel.hpp"
#include "features/channel/core/message-queue.hpp"

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace orangutan;

namespace {

class MockChannel final : public Channel {
public:
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

    void send_message(const std::string &jid, const std::string &text) override {
        sent_messages_.emplace_back(jid, text);
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

    void emit(const InboundMessage &msg) const {
        ASSERT_TRUE(static_cast<bool>(on_message_));
        on_message_(msg);
    }

    [[nodiscard]]
    const std::vector<std::pair<std::string, std::string>> &sent_messages() const {
        return sent_messages_;
    }

private:
    std::string name_;
    std::string jid_prefix_;
    MessageCallback on_message_;
    bool connected_ = false;
    std::vector<std::pair<std::string, std::string>> sent_messages_;
};

} // namespace

TEST(AllowlistTest, EmptyAllowlistAllowsAnyJid) {
    const Allowlist allowlist({}, {});

    EXPECT_TRUE(allowlist.is_allowed("cli:local"));
    EXPECT_TRUE(allowlist.is_allowed("qqbot:c2c:ABC123"));
    EXPECT_TRUE(allowlist.is_allowed("qqbot:group:XYZ789"));
}

TEST(AllowlistTest, AllowPatternsSupportTerminalWildcard) {
    const Allowlist allowlist({"cli:*", "qqbot:c2c:*"}, {});

    EXPECT_TRUE(allowlist.is_allowed("cli:local"));
    EXPECT_TRUE(allowlist.is_allowed("qqbot:c2c:ABC123"));
    EXPECT_FALSE(allowlist.is_allowed("qqbot:group:ABC123"));
}

TEST(AllowlistTest, DenyPatternsOverrideAllowRules) {
    const Allowlist allowlist({"qqbot:c2c:*"}, {"qqbot:c2c:BLOCKED"});

    EXPECT_TRUE(allowlist.is_allowed("qqbot:c2c:ALLOWED"));
    EXPECT_FALSE(allowlist.is_allowed("qqbot:c2c:BLOCKED"));
}

TEST(MessageQueueTest, TryPopReturnsQueuedMessage) {
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
    ASSERT_TRUE(queue.try_pop(message, std::chrono::milliseconds(10)));
    EXPECT_EQ(message.jid, "qqbot:c2c:123");
    EXPECT_EQ(message.content, "hello");
}

TEST(MessageQueueTest, TryPopTimesOutWhenQueueIsEmpty) {
    MessageQueue queue;
    InboundMessage message;

    EXPECT_FALSE(queue.try_pop(message, std::chrono::milliseconds(20)));
}

TEST(MessageQueueTest, ShutdownUnblocksWaitingTryPop) {
    MessageQueue queue;
    auto future = std::async(std::launch::async, [&queue] {
        InboundMessage message;
        return queue.try_pop(message, std::chrono::seconds(5));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.shutdown();

    EXPECT_FALSE(future.get());
}

TEST(ChannelManagerTest, ConnectRoutesInboundMessagesAndOutboundReplies) {
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

    ASSERT_TRUE(manager.has_channels());
    ASSERT_TRUE(cli->is_connected());
    ASSERT_TRUE(qq->is_connected());

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

    ASSERT_EQ(received.size(), 2);
    EXPECT_EQ(received[0].jid, "cli:local");
    EXPECT_EQ(received[1].jid, "qqbot:c2c:456");

    manager.send("cli:local", "reply to cli");
    manager.send("qqbot:c2c:456", "reply to qq");

    ASSERT_EQ(cli->sent_messages().size(), 1);
    EXPECT_EQ(cli->sent_messages()[0].first, "cli:local");
    EXPECT_EQ(cli->sent_messages()[0].second, "reply to cli");

    ASSERT_EQ(qq->sent_messages().size(), 1);
    EXPECT_EQ(qq->sent_messages()[0].first, "qqbot:c2c:456");
    EXPECT_EQ(qq->sent_messages()[0].second, "reply to qq");

    manager.disconnect_all();
    EXPECT_FALSE(cli->is_connected());
    EXPECT_FALSE(qq->is_connected());
}
