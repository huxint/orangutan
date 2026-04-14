#include "providers/provider.hpp"
#include "test-provider-support.hpp"
#include "utils/sender-utils.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

    TEST_CASE("request_builder_forwards_route_request_options_and_events") {
        auto backend = orangutan::testing::make_fake_provider_backend(
            [](const orangutan::providers::ProviderRoute &route, const orangutan::providers::ProviderRequest &request, const orangutan::providers::ProviderEventSink &sink) {
                CHECK(route.primary.model == "gpt-builder");
                CHECK(request.system_prompt == "system prompt");
                REQUIRE(request.messages.size() == 2UL);
                CHECK(request.messages[0].role() == orangutan::base::role::user);
                CHECK(request.messages[1].role() == orangutan::base::role::assistant);
                REQUIRE(request.tools.size() == 1UL);
                CHECK(request.tools[0].name == "lookup");
                CHECK(request.options.max_tokens == 2048);
                CHECK(request.options.thinking_budget == 512);
                CHECK(request.options.timeout_seconds == 42);
                CHECK(request.options.stream);

                if (sink != nullptr) {
                    sink(orangutan::providers::TextDelta{.text = "hello"});
                }

                return orangutan::providers::ProviderResult{
                    .response = orangutan::testing::make_text_response("final"),
                    .usage_snapshot = {},
                    .active_target = route.primary,
                };
            });

        orangutan::ProviderSystem provider(backend);
        const auto route = orangutan::testing::make_test_route("gpt-builder");
        const std::array<orangutan::Message, 1> history = {orangutan::Message::user().text("hello")};
        const std::array<orangutan::ToolDef, 1> tools = {orangutan::ToolDef{
            .name = "lookup",
            .description = "lookup data",
            .input_schema = nlohmann::json::object(),
        }};

        std::string streamed_text;
        const auto result = provider.route(route)
                                .system("system prompt")
                                .messages(history)
                                .append_message(orangutan::Message::assistant().text("working"))
                                .tools(tools)
                                .max_tokens(2048)
                                .thinking_budget(512)
                                .timeout(42)
                                .stream()
                                .on_event([&streamed_text](const orangutan::ProviderEvent &event) {
                                    const auto *delta = std::get_if<orangutan::TextDelta>(&event);
                                    REQUIRE(delta != nullptr);
                                    streamed_text += delta->text;
                                })
                                .send_blocking();

        CHECK(streamed_text == "hello");
        CHECK(result.response.stop_reason == orangutan::response_stop_reason::end_turn);
        CHECK(result.active_target.model == "gpt-builder");
        CHECK(provider.current_model() == "gpt-builder");
    }

    TEST_CASE("request_builder_send_matches_send_blocking_results") {
        auto backend = orangutan::testing::make_fake_provider_backend(
            [](const orangutan::providers::ProviderRoute &route, const orangutan::providers::ProviderRequest &, const orangutan::providers::ProviderEventSink &) {
                return orangutan::providers::ProviderResult{
                    .response = orangutan::testing::make_text_response("consistent"),
                    .usage_snapshot = {
                        .logical_requests = 1,
                        .attempt_count = 1,
                    },
                    .active_target = route.primary,
                };
            });

        orangutan::ProviderSystem provider(backend);
        const auto route = orangutan::testing::make_test_route("gpt-consistent");

        auto async_sender = provider.route(route).system("prompt").send();
        auto [async_result] = orangutan::execution::sync_wait_or_throw(std::move(async_sender), "provider sender test");
        const auto blocking_result = provider.route(route).system("prompt").send_blocking();

        CHECK(async_result.response.content.size() == 1UL);
        CHECK(blocking_result.response.content.size() == 1UL);
        CHECK(async_result.active_target.model == blocking_result.active_target.model);
        CHECK(async_result.usage_snapshot.attempt_count == blocking_result.usage_snapshot.attempt_count);
    }

} // namespace
