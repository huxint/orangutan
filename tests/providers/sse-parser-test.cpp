#include "providers/sse-parser.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

using namespace orangutan;

TEST_CASE("reassembles_fragmented_events_across_chunks") {
    std::vector<std::pair<std::string, std::string>> events;
    SseParser parser([&events](const std::string &event, const std::string &data) {
        events.emplace_back(event, data);
    });

    const std::string first_chunk = "event: message\ndata: {\"par";
    const std::string second_chunk = "tial\":1}\n\n";
    parser.feed(first_chunk);
    parser.feed(second_chunk);

    INFO("expected one parsed event");
    CHECK(events.size() == 1UL);
    CHECK(events[0].first == "message");
    CHECK(events[0].second == "{\"partial\":1}");
};

TEST_CASE("concatenates_multiple_data_lines_into_single_event") {
    std::vector<std::pair<std::string, std::string>> events;
    SseParser parser([&events](const std::string &event, const std::string &data) {
        events.emplace_back(event, data);
    });

    parser.feed("event: chunk\ndata: first line\ndata: second line\n\n");

    INFO("expected one parsed event");
    CHECK(events.size() == 1UL);
    CHECK(events[0].first == "chunk");
    CHECK(events[0].second == "first line\nsecond line");
};

TEST_CASE("json_sse_accumulator_ignores_done_and_invalid_payload") {
    class TestAccumulator final : public orangutan::providers::JsonSseAccumulator<TestAccumulator> {
    public:
        [[nodiscard]]
        std::string_view parse_error_context() const {
            return "test payload";
        }

        void handle_parsed_payload(const nlohmann::json &payload) {
            values.push_back(payload.value("value", -1));
        }

        std::vector<int> values;
    };

    TestAccumulator accumulator;
    accumulator.handle_data("[DONE]");
    accumulator.handle_data("{invalid-json");
    accumulator.handle_data(R"({"value":7})");

    REQUIRE(accumulator.values.size() == 1UL);
    CHECK(accumulator.values[0] == 7);
}

TEST_CASE("json_sse_accumulator_supports_named_events") {
    class TestAccumulator final : public orangutan::providers::JsonSseAccumulator<TestAccumulator> {
    public:
        [[nodiscard]]
        std::string_view parse_error_context() const {
            return "test event payload";
        }

        void handle_parsed_payload(std::string_view event_name, const nlohmann::json &payload) {
            events.emplace_back(std::string{event_name}, payload.value("delta", std::string{}));
        }

        std::vector<std::pair<std::string, std::string>> events;
    };

    TestAccumulator accumulator;
    accumulator.handle_event("response.output_text.delta", R"({"delta":"hello"})");

    REQUIRE(accumulator.events.size() == 1UL);
    CHECK(accumulator.events[0].first == "response.output_text.delta");
    CHECK(accumulator.events[0].second == "hello");
}
