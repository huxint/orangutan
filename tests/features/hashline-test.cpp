#include "features/tools/core/hashline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace orangutan;

static std::string valid_hash_different_from(std::string_view actual) {
    constexpr std::string_view alphabet = "ZPMQVRWSNKTXJBYH";
    for (char first : alphabet) {
        for (char second : alphabet) {
            std::string candidate;
            candidate.push_back(first);
            candidate.push_back(second);
            if (candidate != actual) {
                return candidate;
            }
        }
    }
    throw std::runtime_error("hash alphabet unexpectedly empty");
}

static std::string anchor_for(const std::string &content, std::size_t line_number) {
    return std::to_string(line_number) + "#" + compute_line_hash(content, line_number);
}

// ── Hash computation ────────────────────────────

TEST_CASE("compute_line_hash_returns_2_char_string") {
    const auto hash = compute_line_hash("int x = 42;", 1);
    CHECK(hash.size() == 2ul);
};

TEST_CASE("hash_chars_are_from_alphabet") {
    const std::string alphabet = "ZPMQVRWSNKTXJBYH";
    const auto hash = compute_line_hash("return 0;", 1);
    CHECK(alphabet.contains(hash[0]));
    CHECK(alphabet.contains(hash[1]));
};

TEST_CASE("same_content_same_seed_produces_same_hash") {
    const auto h1 = compute_line_hash("int x = 1;", 1);
    const auto h2 = compute_line_hash("int x = 1;", 2);
    // seed=0 for alphanumeric lines regardless of line_number
    CHECK(h1 == h2);
};

TEST_CASE("symbol_only_lines_use_different_seeds") {
    const auto h1 = compute_line_hash("{", 1);
    const auto h2 = compute_line_hash("{", 5);
    // symbol-only lines use line_number as seed, so different positions differ
    CHECK(h1 != h2);
};

TEST_CASE("empty_line_is_symbol_only") {
    const auto h1 = compute_line_hash("", 1);
    const auto h2 = compute_line_hash("", 3);
    CHECK(h1 != h2);
};

TEST_CASE("trailing_whitespace_does_not_affect_hash") {
    const auto h1 = compute_line_hash("int x;", 1);
    const auto h2 = compute_line_hash("int x;   ", 1);
    CHECK(h1 == h2);
};

TEST_CASE("carriage_return_stripped_before_hash") {
    const auto h1 = compute_line_hash("int x;", 1);
    const auto h2 = compute_line_hash("int x;\r", 1);
    CHECK(h1 == h2);
};

// ── Format hashline ─────────────────────────────

TEST_CASE("format_hashline_produces_expected_format") {
    const auto line = format_hashline("int x = 1;", 5);
    // Should be "5#XX:int x = 1;" where XX is the 2-char hash
    CHECK(line.starts_with("5#"));
    CHECK(line.contains(':'));
    const auto colon_pos = line.find(':');
    CHECK(line.substr(colon_pos + 1) == "int x = 1;");
};

TEST_CASE("format_hashline_hash_part_is_2_chars") {
    const auto line = format_hashline("return 0;", 10);
    // Format: "10#XX:return 0;"
    const auto hash_start = line.find('#') + 1;
    const auto colon_pos = line.find(':');
    CHECK(colon_pos - hash_start == 2ul);
};

// ── Anchor parsing ──────────────────────────────

TEST_CASE("parse_anchor_valid_format") {
    const auto anchor = parse_anchor("42#KQ");
    CHECK(anchor.line == 42ul);
    CHECK(anchor.hash == "KQ");
};

TEST_CASE("parse_anchor_single_digit_line") {
    const auto anchor = parse_anchor("1#ZZ");
    CHECK(anchor.line == 1UL);
    CHECK(anchor.hash == "ZZ");
};

TEST_CASE("parse_anchor_throws_on_missing_hash") {
    REQUIRE_THROWS_AS(parse_anchor("42"), std::runtime_error);
};

TEST_CASE("parse_anchor_throws_on_non_numeric_line") {
    REQUIRE_THROWS_AS(parse_anchor("abc#KQ"), std::runtime_error);
};

TEST_CASE("parse_anchor_throws_on_zero_line") {
    REQUIRE_THROWS_AS(parse_anchor("0#KQ"), std::runtime_error);
};

TEST_CASE("parse_anchor_throws_on_bad_hash_chars") {
    REQUIRE_THROWS_AS(parse_anchor("5#AB"), std::runtime_error);
};

TEST_CASE("parse_anchor_throws_on_empty_string") {
    REQUIRE_THROWS_AS(parse_anchor(""), std::runtime_error);
};

// ── Anchor validation ───────────────────────────

TEST_CASE("validate_anchor_matching_hash") {
    const std::vector<std::string> lines = {"int x = 1;", "int y = 2;", "return x + y;"};
    const auto expected_hash = compute_line_hash("int y = 2;", 2);
    const Anchor anchor{.line = 2, .hash = expected_hash};
    const auto mismatch = validate_anchor(anchor, lines);
    CHECK_FALSE(mismatch.has_value());
};

TEST_CASE("validate_anchor_wrong_hash") {
    const std::vector<std::string> lines = {"int x = 1;", "int y = 2;"};
    const auto actual_hash = compute_line_hash("int y = 2;", 2);
    const Anchor anchor{.line = 2, .hash = valid_hash_different_from(actual_hash)};
    const auto mismatch = validate_anchor(anchor, lines);
    REQUIRE(mismatch.has_value());
    if (mismatch) {
        CHECK(mismatch->line == 2ul);
        CHECK(mismatch->expected == anchor.hash);
        CHECK(mismatch->actual == actual_hash);
    }
};

TEST_CASE("validate_anchor_out_of_range") {
    const std::vector<std::string> lines = {"only one line"};
    const Anchor anchor{.line = 5, .hash = "KQ"};
    const auto mismatch = validate_anchor(anchor, lines);
    REQUIRE(mismatch.has_value());
    if (mismatch) {
        CHECK(mismatch->line == 5ul);
    }
};

// ── Edit operations ─────────────────────────────

TEST_CASE("replace_single_line") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {"XXX"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 3ul);
    CHECK(result.lines[1] == "XXX");
};

TEST_CASE("replace_range") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .end_anchor = anchor_for("ccc", 3),
        .content = {"NEW"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 3ul);
    CHECK(result.lines[0] == "aaa");
    CHECK(result.lines[1] == "NEW");
    CHECK(result.lines[2] == "ddd");
};

TEST_CASE("insert_after_with_anchor") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::insert_after,
        .anchor = anchor_for("aaa", 1),
        .content = {"INSERTED"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 3ul);
    CHECK(result.lines[1] == "INSERTED");
};

TEST_CASE("insert_after_without_anchor_appends_to_eof") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::insert_after,
        .content = {"APPENDED"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 3ul);
    CHECK(result.lines[2] == "APPENDED");
};

TEST_CASE("insert_before_with_anchor") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::insert_before,
        .anchor = anchor_for("bbb", 2),
        .content = {"INSERTED"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 3ul);
    CHECK(result.lines[1] == "INSERTED");
    CHECK(result.lines[2] == "bbb");
};

TEST_CASE("insert_before_without_anchor_prepends_to_bof") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::insert_before,
        .content = {"PREPENDED"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 3ul);
    CHECK(result.lines[0] == "PREPENDED");
};

TEST_CASE("delete_single_line") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::del,
        .anchor = anchor_for("bbb", 2),
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 2ul);
    CHECK(result.lines[0] == "aaa");
    CHECK(result.lines[1] == "ccc");
};

TEST_CASE("delete_range") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::del,
        .anchor = anchor_for("bbb", 2),
        .end_anchor = anchor_for("ccc", 3),
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 2ul);
    CHECK(result.lines[0] == "aaa");
    CHECK(result.lines[1] == "ddd");
};

TEST_CASE("multiple_edits_bottom_up") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
    const std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .content = {"BBB"}},
        {.op = HashlineEditOp::replace, .anchor = anchor_for("ddd", 4), .content = {"DDD"}},
    };
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines[1] == "BBB");
    CHECK(result.lines[3] == "DDD");
};

TEST_CASE("delete_and_insert_after_same_anchor_keeps_insertion_at_deleted_location") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd"};
    const std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::del, .anchor = anchor_for("ccc", 3)},
        {.op = HashlineEditOp::insert_after, .anchor = anchor_for("ccc", 3), .content = {"XXX"}},
    };
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 4ul);
    CHECK(result.lines[0] == "aaa");
    CHECK(result.lines[1] == "bbb");
    CHECK(result.lines[2] == "XXX");
    CHECK(result.lines[3] == "ddd");
};

TEST_CASE("replace_and_insert_after_same_anchor_uses_replacement_boundary") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
    const std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .content = {"BBB-1", "BBB-2"}},
        {.op = HashlineEditOp::insert_after, .anchor = anchor_for("bbb", 2), .content = {"TAIL"}},
    };
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 5ul);
    CHECK(result.lines[0] == "aaa");
    CHECK(result.lines[1] == "BBB-1");
    CHECK(result.lines[2] == "BBB-2");
    CHECK(result.lines[3] == "TAIL");
    CHECK(result.lines[4] == "ccc");
};

TEST_CASE("range_replace_accepts_boundary_inserts") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
    const std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .end_anchor = anchor_for("ddd", 4), .content = {"MID"}},
        {.op = HashlineEditOp::insert_before, .anchor = anchor_for("bbb", 2), .content = {"PRE"}},
        {.op = HashlineEditOp::insert_after, .anchor = anchor_for("ddd", 4), .content = {"POST"}},
    };
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 5ul);
    CHECK(result.lines[0] == "aaa");
    CHECK(result.lines[1] == "PRE");
    CHECK(result.lines[2] == "MID");
    CHECK(result.lines[3] == "POST");
    CHECK(result.lines[4] == "eee");
};

TEST_CASE("replace_with_empty_content_deletes_line") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines.size() == 2ul);
    CHECK(result.lines[0] == "aaa");
    CHECK(result.lines[1] == "ccc");
};

// ── Error cases ─────────────────────────────────

TEST_CASE("hash_mismatch_returns_error") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const auto actual_hash = compute_line_hash("bbb", 2);
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = "2#" + valid_hash_different_from(actual_hash),
        .content = {"XXX"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    CHECK_FALSE(result.ok);
    CHECK(result.error.contains("mismatch"));
};

TEST_CASE("hash_mismatch_error_includes_context_lines") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
    const auto actual_hash = compute_line_hash("ccc", 3);
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = "3#" + valid_hash_different_from(actual_hash),
        .content = {"XXX"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    CHECK_FALSE(result.ok);
    CHECK(result.error.contains(">>>"));
    CHECK(result.error.contains(actual_hash));
    CHECK(result.error.contains("bbb"));
    CHECK(result.error.contains("ddd"));
};

TEST_CASE("overlapping_edits_returns_error") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
    const std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .end_anchor = anchor_for("ddd", 4), .content = {"X"}},
        {.op = HashlineEditOp::del, .anchor = anchor_for("ccc", 3)},
    };
    const auto result = apply_hashline_edits(lines, edits);
    CHECK_FALSE(result.ok);
    CHECK(result.error.contains("overlapping"));
};

TEST_CASE("conflicting_edits_returns_error") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const auto anchor = anchor_for("bbb", 2);
    const std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = anchor, .content = {"XXX"}},
        {.op = HashlineEditOp::replace, .anchor = anchor, .content = {"YYY"}},
    };
    const auto result = apply_hashline_edits(lines, edits);
    CHECK_FALSE(result.ok);
    CHECK(result.error.contains("conflicting"));
};

TEST_CASE("invalid_range_returns_error") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("ccc", 3),
        .end_anchor = anchor_for("aaa", 1),
        .content = {"X"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    CHECK_FALSE(result.ok);
    CHECK(result.error.contains("<="));
};

TEST_CASE("insert_inside_replace_range_returns_error") {
    const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
    const std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .end_anchor = anchor_for("ddd", 4), .content = {"MID"}},
        {.op = HashlineEditOp::insert_after, .anchor = anchor_for("ccc", 3), .content = {"TAIL"}},
    };
    const auto result = apply_hashline_edits(lines, edits);
    CHECK_FALSE(result.ok);
    CHECK(result.error.contains("inside edit range 2-4"));
};

// ── Content auto-stripping ──────────────────────

TEST_CASE("content_auto_stripping_removes_hash_prefixes") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {"10#KQ:replaced line"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines[1] == "replaced line");
    CHECK(result.warnings.contains("stripped"));
};

TEST_CASE("partial_hash_prefix_does_not_strip") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {"10#KQ:line with prefix", "plain line without prefix"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines[1] == "10#KQ:line with prefix");
    CHECK(result.lines[2] == "plain line without prefix");
};

// ── Noop detection ──────────────────────────────

TEST_CASE("noop_replace_same_content") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {"bbb"},
    }};
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.warnings.contains("Noop"));
};

// ── Deduplication ───────────────────────────────

TEST_CASE("duplicate_identical_edits_are_deduplicated") {
    const std::vector<std::string> lines = {"aaa", "bbb"};
    const auto anchor = anchor_for("bbb", 2);
    const std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = anchor, .content = {"XXX"}},
        {.op = HashlineEditOp::replace, .anchor = anchor, .content = {"XXX"}},
    };
    const auto result = apply_hashline_edits(lines, edits);
    REQUIRE(result.ok);
    CHECK(result.lines[1] == "XXX");
};
