#include "core/providers/sse-parser.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

using namespace orangutan;

TEST(SseParserTest, ReassemblesFragmentedEventsAcrossChunks) {
    std::vector<std::pair<std::string, std::string>> events;
    SseParser parser([&events](const std::string &event, const std::string &data) {
        events.emplace_back(event, data);
    });

    const std::string first_chunk = "event: message\ndata: {\"par";
    const std::string second_chunk = "tial\":1}\n\n";
    parser.feed(first_chunk.data(), first_chunk.size());
    parser.feed(second_chunk.data(), second_chunk.size());

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].first, "message");
    EXPECT_EQ(events[0].second, "{\"partial\":1}");
}

TEST(SseParserTest, ConcatenatesMultipleDataLinesIntoSingleEvent) {
    std::vector<std::pair<std::string, std::string>> events;
    SseParser parser([&events](const std::string &event, const std::string &data) {
        events.emplace_back(event, data);
    });

    parser.feed("event: chunk\ndata: first line\ndata: second line\n\n", 51);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].first, "chunk");
    EXPECT_EQ(events[0].second, "first line\nsecond line");
}
