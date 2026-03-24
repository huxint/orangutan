#include "features/channel/core/allowlist.hpp"
#include "features/channel/core/channel.hpp"
#include "features/channel/core/message-queue.hpp"

#include <chrono>
#include <future>
#include "support/ut.hpp"
#include <memory>
#include <stdexcept>
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
        if (!on_message_) {
            throw std::runtime_error("mock channel not connected");
        }
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

boost::ut::suite channel_core_suite = [] {
    using namespace boost::ut;

    "empty_allowlist_allows_any_jid"_test = [] {
        const Allowlist allowlist({}, {});

        expect(allowlist.is_allowed("cli:local"));
        expect(allowlist.is_allowed("qqbot:c2c:ABC123"));
        expect(allowlist.is_allowed("qqbot:group:XYZ789"));
    };

    "allow_patterns_support_terminal_wildcard"_test = [] {
        const Allowlist allowlist({"cli:*", "qqbot:c2c:*"}, {});

        expect(allowlist.is_allowed("cli:local"));
        expect(allowlist.is_allowed("qqbot:c2c:ABC123"));
        expect(not allowlist.is_allowed("qqbot:group:ABC123"));
    };

    "deny_patterns_override_allow_rules"_test = [] {
        const Allowlist allowlist({"qqbot:c2c:*"}, {"qqbot:c2c:BLOCKED"});

        expect(allowlist.is_allowed("qqbot:c2c:ALLOWED"));
        expect(not allowlist.is_allowed("qqbot:c2c:BLOCKED"));
    };

    "try_pop_returns_queued_message"_test = [] {
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
        expect(queue.try_pop(message, std::chrono::milliseconds(10)) >> fatal);
        expect(message.jid == "qqbot:c2c:123");
        expect(message.content == "hello");
    };

    "try_pop_times_out_when_queue_is_empty"_test = [] {
        MessageQueue queue;
        InboundMessage message;

        expect(not queue.try_pop(message, std::chrono::milliseconds(20)));
    };

    "shutdown_unblocks_waiting_try_pop"_test = [] {
        MessageQueue queue;
        auto future = std::async(std::launch::async, [&queue] {
            InboundMessage message;
            return queue.try_pop(message, std::chrono::seconds(5));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        queue.shutdown();

        expect(not future.get());
    };

    "connect_routes_inbound_messages_and_outbound_replies"_test = [] {
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

        expect(manager.has_channels() >> fatal);
        expect(cli->is_connected());
        expect(qq->is_connected());

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

        expect(received.size() == 2_ul);
        expect(received[0].jid == "cli:local");
        expect(received[1].jid == "qqbot:c2c:456");

        manager.send("cli:local", "reply to cli");
        manager.send("qqbot:c2c:456", "reply to qq");

        expect(cli->sent_messages().size() == 1_ul);
        expect(cli->sent_messages()[0].first == "cli:local");
        expect(cli->sent_messages()[0].second == "reply to cli");

        expect(qq->sent_messages().size() == 1_ul);
        expect(qq->sent_messages()[0].first == "qqbot:c2c:456");
        expect(qq->sent_messages()[0].second == "reply to qq");

        manager.disconnect_all();
        expect(not cli->is_connected());
        expect(not qq->is_connected());
    };
};

} // namespace
