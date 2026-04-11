#include "utils/escape.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

TEST_CASE("shell_single_quote_escape wraps and escapes apostrophes") {
    CHECK(utils::shell_single_quote_escape("it's time") == "'it'\\''s time'");
}

TEST_CASE("shell_single_quote_escape preserves empty strings") {
    CHECK(utils::shell_single_quote_escape("") == "''");
}

TEST_CASE("escape_xml encodes reserved characters") {
    CHECK(utils::escape_xml("&<>\"'") == "&amp;&lt;&gt;&quot;&apos;");
}
