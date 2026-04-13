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
        REQUIRE(team.description == "A test team");
        REQUIRE(team.lead_agent_id == "lead-1");
        REQUIRE(team.created_at > 0);
        REQUIRE(team.active);

        auto found = manager.find_team(team.id);
        REQUIRE(found.has_value());
        REQUIRE(found->id == team.id);
        REQUIRE(found->name == "test-team");
        REQUIRE(found->description == "A test team");
        REQUIRE(found->lead_agent_id == "lead-1");
        REQUIRE(found->created_at == team.created_at);
        REQUIRE(found->active);

        auto found_by_name = manager.find_team_by_name("test-team");
        REQUIRE(found_by_name.has_value());
        REQUIRE(found_by_name->id == team.id);
        REQUIRE(found_by_name->description == "A test team");
        REQUIRE(found_by_name->lead_agent_id == "lead-1");
        REQUIRE(found_by_name->created_at == team.created_at);
        REQUIRE(found_by_name->active);
    }

    SECTION("add and list members") {
        auto team = manager.create_team("test-team", "", "lead-1");
        manager.add_member({.agent_id = "agent-1", .name = "worker1", .agent_key = "general-purpose", .team_id = team.id, .joined_at = 42});
        manager.add_member({.agent_id = "agent-2", .name = "worker2", .agent_key = "planner", .team_id = team.id, .joined_at = 84});

        auto members = manager.list_members(team.id);
        REQUIRE(members.size() == 2);
        REQUIRE(members[0].agent_id == "agent-1");
        REQUIRE(members[0].name == "worker1");
        REQUIRE(members[0].agent_key == "general-purpose");
        REQUIRE(members[0].team_id == team.id);
        REQUIRE(members[0].joined_at == 42);
        REQUIRE(members[0].active);
        REQUIRE(members[1].agent_id == "agent-2");
        REQUIRE(members[1].name == "worker2");
        REQUIRE(members[1].agent_key == "planner");
        REQUIRE(members[1].team_id == team.id);
        REQUIRE(members[1].joined_at == 84);
        REQUIRE(members[1].active);
        REQUIRE(manager.list_member_names(team.id) == std::vector<std::string>{"worker1", "worker2"});
    }

    SECTION("deactivate and abandon members") {
        auto team = manager.create_team("test-team", "", "lead-1");
        manager.add_member({.agent_id = "agent-1", .name = "worker1", .agent_key = "general-purpose", .team_id = team.id});
        manager.add_member({.agent_id = "agent-2", .name = "worker2", .agent_key = "planner", .team_id = team.id});

        manager.deactivate_member(team.id, "agent-1");
        auto active_members = manager.list_members(team.id);
        REQUIRE(active_members.size() == 1);
        REQUIRE(active_members[0].agent_id == "agent-2");
        REQUIRE(manager.list_member_names(team.id) == std::vector<std::string>{"worker2"});

        manager.abandon_active_members(team.id);
        REQUIRE(manager.list_members(team.id).empty());
    }

    SECTION("delete team removes members and active listing") {
        auto alpha = manager.create_team("alpha", "", "lead-1");
        auto beta = manager.create_team("beta", "", "lead-2");
        manager.add_member({.agent_id = "agent-1", .name = "worker1", .agent_key = "general-purpose", .team_id = alpha.id});

        const auto active_teams = manager.list_active_teams();
        REQUIRE(active_teams.size() == 2);
        CHECK(active_teams[0].active);
        CHECK(active_teams[1].active);

        manager.delete_team(alpha.id);

        CHECK_FALSE(manager.find_team(alpha.id).has_value());
        CHECK(manager.list_members(alpha.id).empty());

        const auto remaining_teams = manager.list_active_teams();
        REQUIRE(remaining_teams.size() == 1);
        REQUIRE(remaining_teams[0].id == beta.id);
    }
}
