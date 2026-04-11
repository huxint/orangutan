#include <catch2/catch_test_macros.hpp>
#include "swarm/mailbox.hpp"

#include <filesystem>
#include <type_traits>

namespace {

    using MailboxSignature = void (orangutan::swarm::AgentMailbox::*)(const std::string &, const std::string &, const std::string &, const std::string &,
                                                                      orangutan::swarm::message_type);

    static_assert(std::is_constructible_v<orangutan::swarm::AgentMailbox, const std::filesystem::path &>);
    static_assert(std::is_same_v<decltype(&orangutan::swarm::AgentMailbox::send), MailboxSignature>);

} // namespace

TEST_CASE("AgentMailbox basic operations", "[swarm]") {
    // Use in-memory SQLite
    orangutan::swarm::AgentMailbox mailbox(std::filesystem::path{":memory:"});

    SECTION("send and poll") {
        mailbox.send("team1", "coordinator", "worker1", "do the thing");
        auto messages = mailbox.poll("team1", "worker1");
        REQUIRE(messages.size() == 1);
        REQUIRE(messages[0].text == "do the thing");
        REQUIRE(messages[0].from == "coordinator");
    }

    SECTION("broadcast") {
        mailbox.send_broadcast("team1", "coordinator", "hello all", {"worker1", "worker2"});
        auto msgs1 = mailbox.poll("team1", "worker1");
        auto msgs2 = mailbox.poll("team1", "worker2");
        REQUIRE(msgs1.size() == 1);
        REQUIRE(msgs2.size() == 1);
    }

    SECTION("mark read") {
        mailbox.send("team1", "coordinator", "worker1", "task");
        auto messages = mailbox.poll("team1", "worker1");
        REQUIRE(messages.size() == 1);
        mailbox.mark_read({messages[0].id});
        auto after = mailbox.poll("team1", "worker1");
        REQUIRE(after.empty());
    }
}
