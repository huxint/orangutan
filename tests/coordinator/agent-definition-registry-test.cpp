#include <catch2/catch_test_macros.hpp>
#include "coordinator/agent-definition-registry.hpp"

TEST_CASE("AgentDefinitionRegistry built-in agents", "[coordinator]") {
    orangutan::coordinator::AgentDefinitionRegistry registry;
    registry.load_builtin_definitions();

    REQUIRE(registry.has("general-purpose"));
    REQUIRE(registry.has("explorer"));
    REQUIRE(registry.has("planner"));

    auto gp = registry.find("general-purpose");
    REQUIRE(gp.has_value());
    REQUIRE(!gp->description.empty());
}
