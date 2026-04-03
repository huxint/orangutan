#include <catch2/catch_test_macros.hpp>
#include "coordinator/coordinator-manager.hpp"

TEST_CASE("CoordinatorManager basic lifecycle", "[coordinator]") {
    orangutan::coordinator::CoordinatorManager manager(2);

    SECTION("starts empty") {
        REQUIRE(manager.list_active_runs().empty());
    }

    SECTION("shutdown is safe when empty") {
        manager.shutdown();
        REQUIRE(manager.list_active_runs().empty());
    }
}
