#include "utils/utf8-policy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>

#include "types/base.hpp"

using namespace orangutan;

static_assert(std::is_same_v<std::underlying_type_t<utf8_policy::invalid_utf8_mode>, base::u8>);
static_assert(std::is_same_v<std::underlying_type_t<utf8_policy::normalization_mode>, base::u8>);
static_assert(std::is_same_v<std::underlying_type_t<utf8_policy::control_character_mode>, base::u8>);
static_assert(std::is_same_v<std::underlying_type_t<utf8_policy::bound_mode>, base::u8>);
static_assert(std::is_same_v<std::underlying_type_t<utf8_policy::canonicalize_error_code>, base::u8>);

TEST_CASE("drop_invalid_mode_keeps_valid_spans") {
    utf8_policy::CanonicalizePolicy policy{};
    policy.invalid_utf8 = utf8_policy::invalid_utf8_mode::drop_invalid;

    const auto output = utf8_policy::canonicalize(std::string{"ok"} + "\xff" + "再见", policy);
    REQUIRE(output.has_value());
    CHECK(output->value == std::string{"ok再见"});
    CHECK(output->changed);
    CHECK_FALSE(output->truncated);
};

TEST_CASE("reject_invalid_mode_returns_error") {
    utf8_policy::CanonicalizePolicy policy{};
    policy.invalid_utf8 = utf8_policy::invalid_utf8_mode::reject;

    const auto output = utf8_policy::canonicalize(std::string{"A"} + "\xff" + "B", policy);
    REQUIRE_FALSE(output.has_value());
    CHECK(output.error().code == utf8_policy::canonicalize_error_code::invalid_utf8);
    CHECK(output.error().byte_offset == 1U);
};

TEST_CASE("replace_invalid_mode_inserts_replacement_character") {
    utf8_policy::CanonicalizePolicy policy{};
    policy.invalid_utf8 = utf8_policy::invalid_utf8_mode::replace_invalid;

    const auto output = utf8_policy::canonicalize(std::string{"A"} + "\xff" + "B", policy);
    REQUIRE(output.has_value());
    CHECK(output->value == std::string{"A"} + "\xef\xbf\xbd" + "B");
};

TEST_CASE("nfkc_casefold_normalizes_for_search_keys") {
    utf8_policy::CanonicalizePolicy policy{};
    policy.normalization = utf8_policy::normalization_mode::nfkc_casefold;

    const auto output = utf8_policy::canonicalize("ＡＢＣ", policy);
    REQUIRE(output.has_value());
    CHECK(output->value == std::string{"abc"});
};

TEST_CASE("strip_control_mode_removes_control_code_points") {
    utf8_policy::CanonicalizePolicy policy{};
    policy.control_characters = utf8_policy::control_character_mode::strip;

    const auto output = utf8_policy::canonicalize(std::string{"a"} + "\x01" + "b\n", policy);
    REQUIRE(output.has_value());
    CHECK(output->value == std::string{"ab"});
};

TEST_CASE("reject_control_mode_returns_error") {
    utf8_policy::CanonicalizePolicy policy{};
    policy.control_characters = utf8_policy::control_character_mode::reject;

    const auto output = utf8_policy::canonicalize(std::string{"a"} + "\x01" + "b", policy);
    REQUIRE_FALSE(output.has_value());
    CHECK(output.error().code == utf8_policy::canonicalize_error_code::disallowed_code_point);
    CHECK(output.error().byte_offset == 1U);
};

TEST_CASE("prefix_bound_respects_code_point_boundary_and_ellipsis") {
    utf8_policy::CanonicalizePolicy policy{};
    policy.boundary = utf8_policy::bound_mode::prefix;
    policy.max_bytes = 9;
    policy.append_ellipsis = true;

    const auto output = utf8_policy::canonicalize("你好世界", policy);
    REQUIRE(output.has_value());
    CHECK(output->value == std::string{"你好..."});
    CHECK(output->truncated);
};

TEST_CASE("suffix_bound_respects_code_point_boundary") {
    utf8_policy::CanonicalizePolicy policy{};
    policy.boundary = utf8_policy::bound_mode::suffix;
    policy.max_bytes = 4;

    const auto output = utf8_policy::canonicalize("ab你好", policy);
    REQUIRE(output.has_value());
    CHECK(output->value == std::string{"好"});
    CHECK(output->truncated);
};

TEST_CASE("display_policy_keeps_newlines_and_truncates_prefix") {
    const auto output = utf8_policy::canonicalize("line1\nline2", utf8_policy::display_policy(8, true));
    REQUIRE(output.has_value());
    CHECK(output->value == std::string{"line1..."});
};
