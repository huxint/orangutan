#include <catch2/catch_test_macros.hpp>
#include "orchestration/mailbox.hpp"

#include <filesystem>
#include <concepts>
#include <type_traits>

namespace {

    using MailboxSignature = void (orangutan::orchestration::AgentMailbox::*)(const std::string &, const std::string &, const std::string &, const std::string &,
                                                                      orangutan::orchestration::message_type);

    static_assert(std::constructible_from<orangutan::orchestration::AgentMailbox, const std::filesystem::path &>);
    static_assert(std::same_as<decltype(&orangutan::orchestration::AgentMailbox::send), MailboxSignature>);

} // namespace

TEST_CASE("AgentMailbox basic operations", "[orchestration]") {
    // Use in-memory SQLite
    orangutan::orchestration::AgentMailbox mailbox(std::filesystem::path{":memory:"});

    SECTION("send and poll") {
        mailbox.send("team1", "leader", "worker1", "do the thing");
        auto messages = mailbox.poll("team1", "worker1");
        REQUIRE(messages.size() == 1);
        REQUIRE(messages[0].text == "do the thing");
        REQUIRE(messages[0].from == "leader");
        REQUIRE(messages[0].type == orangutan::orchestration::message_type::message);
    }

    SECTION("broadcast") {
        mailbox.send_broadcast("team1", "worker1", "hello all", {"worker1", "worker2", "worker3"});
        auto sender_messages = mailbox.poll("team1", "worker1");
        auto msgs1 = mailbox.poll("team1", "worker2");
        auto msgs2 = mailbox.poll("team1", "worker3");
        REQUIRE(sender_messages.empty());
        REQUIRE(msgs1.size() == 1);
        REQUIRE(msgs2.size() == 1);
    }

    SECTION("mark read") {
        mailbox.send("team1", "leader", "worker1", "task");
        auto messages = mailbox.poll("team1", "worker1");
        REQUIRE(messages.size() == 1);
        mailbox.mark_read({messages[0].id});
        auto after = mailbox.poll("team1", "worker1");
        REQUIRE(after.empty());
    }

    SECTION("mark read updates multiple targeted messages") {
        mailbox.send("team1", "leader", "worker1", "task-1");
        mailbox.send("team1", "leader", "worker1", "task-2");
        mailbox.send("team1", "leader", "worker2", "task-3");

        const auto worker1_messages = mailbox.poll("team1", "worker1");
        REQUIRE(worker1_messages.size() == 2);

        mailbox.mark_read({worker1_messages[0].id, worker1_messages[1].id});

        CHECK(mailbox.poll("team1", "worker1").empty());
        CHECK(mailbox.poll("team1", "worker2").size() == 1);
    }

    SECTION("clear team removes unread messages") {
        mailbox.send("team1", "leader", "worker1", "task");
        mailbox.send("team1", "leader", "worker2", "task");
        mailbox.clear_team("team1");
        CHECK(mailbox.poll("team1", "worker1").empty());
        CHECK(mailbox.poll("team1", "worker2").empty());
    }

    SECTION("custom message type round trips") {
        mailbox.send("team1", "leader", "worker1", "shutdown", orangutan::orchestration::message_type::shutdown_request);
        auto messages = mailbox.poll("team1", "worker1");
        REQUIRE(messages.size() == 1);
        CHECK(messages[0].type == orangutan::orchestration::message_type::shutdown_request);
    }
}
