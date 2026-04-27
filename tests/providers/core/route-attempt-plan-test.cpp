#include "providers/execution/route-attempt-plan.hpp"
#include "test-provider-support.hpp"

#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

namespace {

    [[nodiscard]]
    orangutan::providers::ModelTarget target(std::string model, std::string profile = "test-profile") {
        return orangutan::testing::make_test_target(std::move(model), orangutan::provider_kind::openai, orangutan::protocol_kind::responses, "test-key",
                                                   "https://example.test", std::move(profile));
    }

    TEST_CASE("route_attempt_plan_filters_empty_and_duplicate_targets") {
        auto duplicate = target("fallback-a", "duplicate-profile");
        orangutan::providers::ProviderRoute route{
            .primary = target("primary"),
            .fallbacks = {
                orangutan::providers::ModelTarget{},
                duplicate,
                duplicate,
                target("fallback-b", "other-profile"),
            },
        };

        const orangutan::providers::execution::RouteAttemptPlan plan(route, "");

        REQUIRE(plan.targets().size() == 3UL);
        CHECK(plan.targets()[0].model == "primary");
        CHECK(plan.targets()[1].model == "fallback-a");
        CHECK(plan.targets()[2].model == "fallback-b");
        CHECK(plan.current().model == "primary");
        CHECK(plan.current_index() == 0UL);
    }

    TEST_CASE("route_attempt_plan_starts_at_preferred_target_without_wraparound") {
        const auto preferred = target("fallback-b", "preferred-profile");
        orangutan::providers::ProviderRoute route{
            .primary = target("primary"),
            .fallbacks = {
                target("fallback-a", "first-profile"),
                preferred,
            },
        };

        orangutan::providers::execution::RouteAttemptPlan plan(route, orangutan::providers::execution::target_key(preferred));

        CHECK(plan.current().model == "fallback-b");
        CHECK(plan.current_index() == 2UL);
        CHECK_FALSE(plan.can_advance_after(orangutan::ProviderError(orangutan::error_category::network, "network"), false));
    }

    TEST_CASE("route_attempt_plan_advances_only_for_retryable_failures_before_streaming") {
        orangutan::providers::ProviderRoute route{
            .primary = target("primary"),
            .fallbacks = {target("fallback")},
        };

        orangutan::providers::execution::RouteAttemptPlan plan(route, "");

        CHECK(plan.can_advance_after(orangutan::ProviderError(orangutan::error_category::network, "network"), false));
        CHECK_FALSE(plan.can_advance_after(orangutan::ProviderError(orangutan::error_category::authentication, "auth"), false));
        CHECK_FALSE(plan.can_advance_after(orangutan::ProviderError(orangutan::error_category::network, "network"), true));

        plan.advance();
        CHECK(plan.current().model == "fallback");
        CHECK_FALSE(plan.can_advance_after(orangutan::ProviderError(orangutan::error_category::network, "network"), false));
    }

} // namespace
