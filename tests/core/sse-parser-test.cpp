#include "core/providers/sse-parser.hpp"
#include <catch2/catch_test_macros.hpp>
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
    CHECK(events.size() == 1ul);
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
    CHECK(events.size() == 1ul);
    CHECK(events[0].first == "chunk");
    CHECK(events[0].second == "first line\nsecond line");
};
