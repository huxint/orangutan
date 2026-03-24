#include "features/heartbeat/protocol/heartbeat-ok.hpp"

#include "support/ut.hpp"
#include <string>

namespace {

boost::ut::suite heartbeat_ok_suite = [] {
    using namespace boost::ut;

    "exact_match_suppresses"_test = [] {
        expect(orangutan::detect_heartbeat_ok("HEARTBEAT_OK", 300));
    };

    "prefix_with_short_text_suppresses"_test = [] {
        std::string stripped;
        expect(orangutan::detect_heartbeat_ok("HEARTBEAT_OK All clear.", 300, &stripped));
        expect(stripped == "All clear.");
    };

    "suffix_token_suppresses"_test = [] {
        std::string stripped;
        expect(orangutan::detect_heartbeat_ok("Everything looks good.\nHEARTBEAT_OK", 300, &stripped));
        expect(stripped == "Everything looks good.");
    };

    "prefix_with_long_text_does_not_suppress"_test = [] {
        const auto long_text = std::string("HEARTBEAT_OK ") + std::string(400, 'x');
        expect(not orangutan::detect_heartbeat_ok(long_text, 300));
    };

    "token_in_middle_does_not_suppress"_test = [] {
        expect(not orangutan::detect_heartbeat_ok("Something HEARTBEAT_OK something", 300));
    };

    "no_token_does_not_suppress"_test = [] {
        expect(not orangutan::detect_heartbeat_ok("All systems operational", 300));
    };

    "empty_response_does_not_suppress"_test = [] {
        expect(not orangutan::detect_heartbeat_ok("", 300));
    };

    "whitespace_around_token_suppresses"_test = [] {
        expect(orangutan::detect_heartbeat_ok("  HEARTBEAT_OK  ", 300));
    };

    "suppression_requires_heartbeat_jid"_test = [] {
        expect(orangutan::should_suppress_heartbeat_reply("heartbeat:daily", "HEARTBEAT_OK", 300));
        expect(not orangutan::should_suppress_heartbeat_reply("cli", "HEARTBEAT_OK", 300));
    };
};

} // namespace
