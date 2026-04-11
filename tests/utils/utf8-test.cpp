#include "utils/utf8.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace orangutan;

TEST_CASE("ascii_passes_through") {
    CHECK(utf8::sanitize("hello world") == std::string{"hello world"});
    CHECK(utf8::sanitize("") == std::string{""});
};

TEST_CASE("valid_chinese_passes_through") {
    const std::string input = "\xe7\xae\x97\xe6\xb3\x95\xe9\xab\x98\xe6\x89\x8b";
    CHECK(utf8::sanitize(input) == input);
};

TEST_CASE("truncated_chinese_character_is_stripped") {
    const std::string truncated = "\xe7\xae";
    CHECK(utf8::sanitize(truncated) == std::string{""});
};

TEST_CASE("valid_text_followed_by_truncated_character") {
    const std::string input = std::string{"hi"} + "\xe4\xb8";
    CHECK(utf8::sanitize(input) == std::string{"hi"});
};

TEST_CASE("valid_chinese_followed_by_truncated_character") {
    const std::string input = "\xe7\xae\x97\xe6";
    CHECK(utf8::sanitize(input) == std::string{"\xe7\xae\x97"});
};

TEST_CASE("accented_latin_passes_through") {
    const std::string input = "Jos\xc3\xa9";
    CHECK(utf8::sanitize(input) == input);
};

TEST_CASE("truncated_accented_latin_is_stripped") {
    const std::string input = "Jos\xc3";
    CHECK(utf8::sanitize(input) == std::string{"Jos"});
};

TEST_CASE("emoji_passes_through") {
    const std::string input = "\xf0\x9f\x9a\x80";
    CHECK(utf8::sanitize(input) == input);
};

TEST_CASE("truncated_emoji_is_stripped") {
    const std::string input = "go\xf0\x9f\x9a";
    CHECK(utf8::sanitize(input) == std::string{"go"});
};

TEST_CASE("invalid_leading_byte_is_skipped_and_later_text_survives") {
    const std::string input = std::string{"ok"} + "\xff" + "再见";
    CHECK(utf8::sanitize(input) == std::string{"ok再见"});
};

TEST_CASE("stray_continuation_bytes_are_skipped") {
    const std::string input = std::string{"ok"} + "\x80\xbf" + "fine";
    CHECK(utf8::sanitize(input) == std::string{"okfine"});
};
