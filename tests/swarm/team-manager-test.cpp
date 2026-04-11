#include <catch2/catch_test_macros.hpp>
#include "swarm/team-manager.hpp"

#include <filesystem>
#include <type_traits>

namespace {

    static_assert(std::is_constructible_v<orangutan::swarm::TeamManager, const std::filesystem::path &>);

} // namespace

TEST_CASE("TeamManager basic operations", "[swarm]") {
    orangutan::swarm::TeamManager manager(std::filesystem::path{":memory:"});

    SECTION("create and find team") {
        auto team = manager.create_team("test-team", "A test team", "lead-1");
        REQUIRE(!team.id.empty());
        REQUIRE(team.name == "test-team");

        auto found = manager.find_team(team.id);
        REQUIRE(found.has_value());
        REQUIRE(found->name == "test-team");
    }

    SECTION("add and list members") {
        auto team = manager.create_team("test-team", "", "lead-1");
        manager.add_member({.agent_id = "agent-1", .name = "worker1", .agent_key = "general-purpose", .team_id = team.id});

        auto members = manager.list_members(team.id);
        REQUIRE(members.size() == 1);
        REQUIRE(members[0].name == "worker1");
    }
}
