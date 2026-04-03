#include <catch2/catch_test_macros.hpp>
#include "swarm/mailbox.hpp"

TEST_CASE("AgentMailbox basic operations", "[swarm]") {
    // Use in-memory SQLite
    orangutan::swarm::AgentMailbox mailbox(":memory:");

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
