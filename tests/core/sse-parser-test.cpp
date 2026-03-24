#include "core/providers/sse-parser.hpp"
#include "support/ut.hpp"
#include <utility>
#include <vector>

using namespace orangutan;

boost::ut::suite sse_parser_suite = [] {
    using namespace boost::ut;

    "reassembles_fragmented_events_across_chunks"_test = [] {
        std::vector<std::pair<std::string, std::string>> events;
        SseParser parser([&events](const std::string &event, const std::string &data) {
            events.emplace_back(event, data);
        });

        const std::string first_chunk = "event: message\ndata: {\"par";
        const std::string second_chunk = "tial\":1}\n\n";
        parser.feed(first_chunk.data(), first_chunk.size());
        parser.feed(second_chunk.data(), second_chunk.size());

        expect(events.size() == 1_ul) << "expected one parsed event";
        expect(events[0].first == "message");
        expect(events[0].second == "{\"partial\":1}");
    };

    "concatenates_multiple_data_lines_into_single_event"_test = [] {
        std::vector<std::pair<std::string, std::string>> events;
        SseParser parser([&events](const std::string &event, const std::string &data) {
            events.emplace_back(event, data);
        });

        parser.feed("event: chunk\ndata: first line\ndata: second line\n\n", 51);

        expect(events.size() == 1_ul) << "expected one parsed event";
        expect(events[0].first == "chunk");
        expect(events[0].second == "first line\nsecond line");
    };
};
