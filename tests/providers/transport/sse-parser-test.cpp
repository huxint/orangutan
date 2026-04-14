#include "providers/transport/sse-parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

    TEST_CASE("sse_parser_emits_named_events_with_multiline_data") {
        std::vector<std::pair<std::string, std::string>> events;
        orangutan::providers::transport::SseParser parser([&events](std::string_view event_name, std::string_view data) {
            events.emplace_back(event_name, data);
        });

        parser.feed("event: response.output_text.delta\n");
        parser.feed("data: hello\n");
        parser.feed("data: world\n\n");

        REQUIRE(events.size() == 1UL);
        CHECK(events[0].first == "response.output_text.delta");
        CHECK(events[0].second == "hello\nworld");
    }

    TEST_CASE("sse_parser_handles_fragmented_lines_and_default_event_names") {
        std::vector<std::pair<std::string, std::string>> events;
        orangutan::providers::transport::SseParser parser([&events](std::string_view event_name, std::string_view data) {
            events.emplace_back(event_name, data);
        });

        parser.feed("data: part");
        parser.feed("ial\r\n");
        parser.feed("\r\n");

        REQUIRE(events.size() == 1UL);
        CHECK(events[0].first.empty());
        CHECK(events[0].second == "partial");
    }

} // namespace
