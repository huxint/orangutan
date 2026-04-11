#include "utils/string.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace orangutan;

TEST_CASE("trim_copy removes surrounding whitespace") {
    CHECK(utils::trim_copy(" \t hello world \n") == "hello world");
    CHECK(utils::trim_copy("\n\t ") == "");
}

TEST_CASE("ascii_to_lower_copy lowercases ASCII characters") {
    CHECK(utils::ascii_to_lower_copy("HeLLo-123_!?") == "hello-123_!?");
}

TEST_CASE("normalize_enum_token lowercases and normalizes separators") {
    CHECK(utils::normalize_enum_token("Bypass-Permissions_mode") == "bypass_permissions_mode");
}

TEST_CASE("split_trimmed trims comma separated fields and skips blanks") {
    CHECK(utils::split_trimmed(" read  , shell(git:*), , task(list)  ,   ") == std::vector<std::string>{"read", "shell(git:*)", "task(list)"});
}

TEST_CASE("split_trimmed ignores non-comma separators") {
    CHECK(utils::split_trimmed(" read  ; shell(git:*); ; task(list)  ;   ") == std::vector<std::string>{"read  ; shell(git:*); ; task(list)  ;"});
}
