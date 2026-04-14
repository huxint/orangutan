#include "providers/provider.hpp"
#include "test-provider-support.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

    TEST_CASE("provider_core_parses_kinds_and_formats_labels") {
        CHECK(orangutan::parse_provider_kind("openai") == orangutan::provider_kind::openai);
        CHECK(orangutan::parse_provider_kind("Anthropic") == orangutan::provider_kind::anthropic);
        CHECK(orangutan::parse_protocol_kind("chat-completions") == orangutan::protocol_kind::chat_completions);
        CHECK(orangutan::parse_protocol_kind("responses") == orangutan::protocol_kind::responses);
        CHECK(orangutan::parse_protocol_kind("messages") == orangutan::protocol_kind::messages);

        const auto target = orangutan::testing::make_test_target("gpt-5", orangutan::provider_kind::openai, orangutan::protocol_kind::responses);
        CHECK(orangutan::target_label(target) == "openai:responses:gpt-5");
    }

    TEST_CASE("provider_core_maps_stop_reasons") {
        CHECK(orangutan::map_stop_reason("stop") == orangutan::response_stop_reason::end_turn);
        CHECK(orangutan::map_stop_reason("tool_calls") == orangutan::response_stop_reason::tool_use);
        CHECK(orangutan::map_stop_reason("length") == orangutan::response_stop_reason::max_tokens);
        CHECK(orangutan::map_stop_reason("completed") == orangutan::response_stop_reason::end_turn);
        CHECK(orangutan::map_stop_reason("mystery") == orangutan::response_stop_reason::unknown);
    }

    TEST_CASE("provider_error_retryability_matches_execution_policy_expectations") {
        CHECK(orangutan::ProviderError(orangutan::error_category::network, "network").retryable());
        CHECK(orangutan::ProviderError(orangutan::error_category::rate_limit, "rate limit").retryable());
        CHECK(orangutan::ProviderError(orangutan::error_category::upstream, "upstream").retryable());
        CHECK_FALSE(orangutan::ProviderError(orangutan::error_category::configuration, "configuration").retryable());
        CHECK_FALSE(orangutan::ProviderError(orangutan::error_category::authentication, "authentication").retryable());
        CHECK_FALSE(orangutan::ProviderError(orangutan::error_category::parsing, "parsing").retryable());
    }

    TEST_CASE("provider_core_rejects_unknown_kind_tokens") {
        CHECK_THROWS_AS(orangutan::parse_provider_kind("made-up"), orangutan::ProviderError);
        CHECK_THROWS_AS(orangutan::parse_protocol_kind("unsupported"), orangutan::ProviderError);
    }

} // namespace
