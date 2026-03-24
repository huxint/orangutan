#include "features/tools/core/hashline.hpp"

#include "support/ut.hpp"
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

static std::string anchor_for(const std::string &content, size_t line_number) {
    return std::to_string(line_number) + "#" + compute_line_hash(content, line_number);
}

boost::ut::suite hashline_suite = [] {
    using namespace boost::ut;

    // ── Hash computation ────────────────────────────

    "compute_line_hash_returns_2_char_string"_test = [] {
        const auto hash = compute_line_hash("int x = 42;", 1);
        expect(hash.size() == 2_ul);
    };

    "hash_chars_are_from_alphabet"_test = [] {
        const std::string alphabet = "ZPMQVRWSNKTXJBYH";
        const auto hash = compute_line_hash("return 0;", 1);
        expect(alphabet.find(hash[0]) != std::string::npos);
        expect(alphabet.find(hash[1]) != std::string::npos);
    };

    "same_content_same_seed_produces_same_hash"_test = [] {
        const auto h1 = compute_line_hash("int x = 1;", 1);
        const auto h2 = compute_line_hash("int x = 1;", 2);
        // seed=0 for alphanumeric lines regardless of line_number
        expect(h1 == h2);
    };

    "symbol_only_lines_use_different_seeds"_test = [] {
        const auto h1 = compute_line_hash("{", 1);
        const auto h2 = compute_line_hash("{", 5);
        // symbol-only lines use line_number as seed, so different positions differ
        expect(h1 != h2);
    };

    "empty_line_is_symbol_only"_test = [] {
        const auto h1 = compute_line_hash("", 1);
        const auto h2 = compute_line_hash("", 3);
        expect(h1 != h2);
    };

    "trailing_whitespace_does_not_affect_hash"_test = [] {
        const auto h1 = compute_line_hash("int x;", 1);
        const auto h2 = compute_line_hash("int x;   ", 1);
        expect(h1 == h2);
    };

    "carriage_return_stripped_before_hash"_test = [] {
        const auto h1 = compute_line_hash("int x;", 1);
        const auto h2 = compute_line_hash("int x;\r", 1);
        expect(h1 == h2);
    };

    // ── Format hashline ─────────────────────────────

    "format_hashline_produces_expected_format"_test = [] {
        const auto line = format_hashline("int x = 1;", 5);
        // Should be "5#XX:int x = 1;" where XX is the 2-char hash
        expect(line.starts_with("5#"));
        expect(line.find(':') != std::string::npos);
        const auto colon_pos = line.find(':');
        expect(line.substr(colon_pos + 1) == "int x = 1;");
    };

    "format_hashline_hash_part_is_2_chars"_test = [] {
        const auto line = format_hashline("return 0;", 10);
        // Format: "10#XX:return 0;"
        const auto hash_start = line.find('#') + 1;
        const auto colon_pos = line.find(':');
        expect(colon_pos - hash_start == 2_ul);
    };

    // ── Anchor parsing ──────────────────────────────

    "parse_anchor_valid_format"_test = [] {
        const auto anchor = parse_anchor("42#KQ");
        expect(anchor.line == 42_ul);
        expect(anchor.hash == "KQ");
    };

    "parse_anchor_single_digit_line"_test = [] {
        const auto anchor = parse_anchor("1#ZZ");
        expect(anchor.line == 1_ul);
        expect(anchor.hash == "ZZ");
    };

    "parse_anchor_throws_on_missing_hash"_test = [] {
        expect(throws<std::runtime_error>([] {
            static_cast<void>(parse_anchor("42"));
        }));
    };

    "parse_anchor_throws_on_non_numeric_line"_test = [] {
        expect(throws<std::runtime_error>([] {
            static_cast<void>(parse_anchor("abc#KQ"));
        }));
    };

    "parse_anchor_throws_on_zero_line"_test = [] {
        expect(throws<std::runtime_error>([] {
            static_cast<void>(parse_anchor("0#KQ"));
        }));
    };

    "parse_anchor_throws_on_bad_hash_chars"_test = [] {
        expect(throws<std::runtime_error>([] {
            static_cast<void>(parse_anchor("5#AB"));
        }));
    };

    "parse_anchor_throws_on_empty_string"_test = [] {
        expect(throws<std::runtime_error>([] {
            static_cast<void>(parse_anchor(""));
        }));
    };

    // ── Anchor validation ───────────────────────────

    "validate_anchor_matching_hash"_test = [] {
        const std::vector<std::string> lines = {"int x = 1;", "int y = 2;", "return x + y;"};
        const auto expected_hash = compute_line_hash("int y = 2;", 2);
        const Anchor anchor{.line = 2, .hash = expected_hash};
        const auto mismatch = validate_anchor(anchor, lines);
        expect(not mismatch.has_value());
    };

    "validate_anchor_wrong_hash"_test = [] {
        const std::vector<std::string> lines = {"int x = 1;", "int y = 2;"};
        const auto actual_hash = compute_line_hash("int y = 2;", 2);
        const Anchor anchor{.line = 2, .hash = valid_hash_different_from(actual_hash)};
        const auto mismatch = validate_anchor(anchor, lines);
        expect(mismatch.has_value() >> fatal);
        if (mismatch) {
            expect(mismatch->line == 2_ul);
            expect(mismatch->expected == anchor.hash);
            expect(mismatch->actual == actual_hash);
        }
    };

    "validate_anchor_out_of_range"_test = [] {
        const std::vector<std::string> lines = {"only one line"};
        const Anchor anchor{.line = 5, .hash = "KQ"};
        const auto mismatch = validate_anchor(anchor, lines);
        expect(mismatch.has_value() >> fatal);
        if (mismatch) {
            expect(mismatch->line == 5_ul);
        }
    };

    // ── Edit operations ─────────────────────────────

    "replace_single_line"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::replace,
            .anchor = anchor_for("bbb", 2),
            .content = {"XXX"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 3_ul);
        expect(result.lines[1] == "XXX");
    };

    "replace_range"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::replace,
            .anchor = anchor_for("bbb", 2),
            .end_anchor = anchor_for("ccc", 3),
            .content = {"NEW"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 3_ul);
        expect(result.lines[0] == "aaa");
        expect(result.lines[1] == "NEW");
        expect(result.lines[2] == "ddd");
    };

    "insert_after_with_anchor"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::insert_after,
            .anchor = anchor_for("aaa", 1),
            .content = {"INSERTED"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 3_ul);
        expect(result.lines[1] == "INSERTED");
    };

    "insert_after_without_anchor_appends_to_eof"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::insert_after,
            .content = {"APPENDED"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 3_ul);
        expect(result.lines[2] == "APPENDED");
    };

    "insert_before_with_anchor"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::insert_before,
            .anchor = anchor_for("bbb", 2),
            .content = {"INSERTED"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 3_ul);
        expect(result.lines[1] == "INSERTED");
        expect(result.lines[2] == "bbb");
    };

    "insert_before_without_anchor_prepends_to_bof"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::insert_before,
            .content = {"PREPENDED"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 3_ul);
        expect(result.lines[0] == "PREPENDED");
    };

    "delete_single_line"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::del,
            .anchor = anchor_for("bbb", 2),
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 2_ul);
        expect(result.lines[0] == "aaa");
        expect(result.lines[1] == "ccc");
    };

    "delete_range"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::del,
            .anchor = anchor_for("bbb", 2),
            .end_anchor = anchor_for("ccc", 3),
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 2_ul);
        expect(result.lines[0] == "aaa");
        expect(result.lines[1] == "ddd");
    };

    "multiple_edits_bottom_up"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
        const std::vector<HashlineEdit> edits = {
            {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .content = {"BBB"}},
            {.op = HashlineEditOp::replace, .anchor = anchor_for("ddd", 4), .content = {"DDD"}},
        };
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines[1] == "BBB");
        expect(result.lines[3] == "DDD");
    };

    "delete_and_insert_after_same_anchor_keeps_insertion_at_deleted_location"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd"};
        const std::vector<HashlineEdit> edits = {
            {.op = HashlineEditOp::del, .anchor = anchor_for("ccc", 3)},
            {.op = HashlineEditOp::insert_after, .anchor = anchor_for("ccc", 3), .content = {"XXX"}},
        };
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 4_ul);
        expect(result.lines[0] == "aaa");
        expect(result.lines[1] == "bbb");
        expect(result.lines[2] == "XXX");
        expect(result.lines[3] == "ddd");
    };

    "replace_and_insert_after_same_anchor_uses_replacement_boundary"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
        const std::vector<HashlineEdit> edits = {
            {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .content = {"BBB-1", "BBB-2"}},
            {.op = HashlineEditOp::insert_after, .anchor = anchor_for("bbb", 2), .content = {"TAIL"}},
        };
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 5_ul);
        expect(result.lines[0] == "aaa");
        expect(result.lines[1] == "BBB-1");
        expect(result.lines[2] == "BBB-2");
        expect(result.lines[3] == "TAIL");
        expect(result.lines[4] == "ccc");
    };

    "range_replace_accepts_boundary_inserts"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
        const std::vector<HashlineEdit> edits = {
            {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .end_anchor = anchor_for("ddd", 4), .content = {"MID"}},
            {.op = HashlineEditOp::insert_before, .anchor = anchor_for("bbb", 2), .content = {"PRE"}},
            {.op = HashlineEditOp::insert_after, .anchor = anchor_for("ddd", 4), .content = {"POST"}},
        };
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 5_ul);
        expect(result.lines[0] == "aaa");
        expect(result.lines[1] == "PRE");
        expect(result.lines[2] == "MID");
        expect(result.lines[3] == "POST");
        expect(result.lines[4] == "eee");
    };

    "replace_with_empty_content_deletes_line"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::replace,
            .anchor = anchor_for("bbb", 2),
            .content = {},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines.size() == 2_ul);
        expect(result.lines[0] == "aaa");
        expect(result.lines[1] == "ccc");
    };

    // ── Error cases ─────────────────────────────────

    "hash_mismatch_returns_error"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const auto actual_hash = compute_line_hash("bbb", 2);
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::replace,
            .anchor = "2#" + valid_hash_different_from(actual_hash),
            .content = {"XXX"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(not result.ok);
        expect(result.error.find("mismatch") != std::string::npos);
    };

    "hash_mismatch_error_includes_context_lines"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
        const auto actual_hash = compute_line_hash("ccc", 3);
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::replace,
            .anchor = "3#" + valid_hash_different_from(actual_hash),
            .content = {"XXX"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(not result.ok);
        expect(result.error.find(">>>") != std::string::npos);
        expect(result.error.find(actual_hash) != std::string::npos);
        expect(result.error.find("bbb") != std::string::npos);
        expect(result.error.find("ddd") != std::string::npos);
    };

    "overlapping_edits_returns_error"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
        const std::vector<HashlineEdit> edits = {
            {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .end_anchor = anchor_for("ddd", 4), .content = {"X"}},
            {.op = HashlineEditOp::del, .anchor = anchor_for("ccc", 3)},
        };
        const auto result = apply_hashline_edits(lines, edits);
        expect(not result.ok);
        expect(result.error.find("overlapping") != std::string::npos);
    };

    "conflicting_edits_returns_error"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const auto anchor = anchor_for("bbb", 2);
        const std::vector<HashlineEdit> edits = {
            {.op = HashlineEditOp::replace, .anchor = anchor, .content = {"XXX"}},
            {.op = HashlineEditOp::replace, .anchor = anchor, .content = {"YYY"}},
        };
        const auto result = apply_hashline_edits(lines, edits);
        expect(not result.ok);
        expect(result.error.find("conflicting") != std::string::npos);
    };

    "invalid_range_returns_error"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::replace,
            .anchor = anchor_for("ccc", 3),
            .end_anchor = anchor_for("aaa", 1),
            .content = {"X"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(not result.ok);
        expect(result.error.find("<=") != std::string::npos);
    };

    "insert_inside_replace_range_returns_error"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
        const std::vector<HashlineEdit> edits = {
            {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .end_anchor = anchor_for("ddd", 4), .content = {"MID"}},
            {.op = HashlineEditOp::insert_after, .anchor = anchor_for("ccc", 3), .content = {"TAIL"}},
        };
        const auto result = apply_hashline_edits(lines, edits);
        expect(not result.ok);
        expect(result.error.find("inside edit range 2-4") != std::string::npos);
    };

    // ── Content auto-stripping ──────────────────────

    "content_auto_stripping_removes_hash_prefixes"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::replace,
            .anchor = anchor_for("bbb", 2),
            .content = {"10#KQ:replaced line"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines[1] == "replaced line");
        expect(result.warnings.find("stripped") != std::string::npos);
    };

    "partial_hash_prefix_does_not_strip"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::replace,
            .anchor = anchor_for("bbb", 2),
            .content = {"10#KQ:line with prefix", "plain line without prefix"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines[1] == "10#KQ:line with prefix");
        expect(result.lines[2] == "plain line without prefix");
    };

    // ── Noop detection ──────────────────────────────

    "noop_replace_same_content"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const std::vector<HashlineEdit> edits = {{
            .op = HashlineEditOp::replace,
            .anchor = anchor_for("bbb", 2),
            .content = {"bbb"},
        }};
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.warnings.find("Noop") != std::string::npos);
    };

    // ── Deduplication ───────────────────────────────

    "duplicate_identical_edits_are_deduplicated"_test = [] {
        const std::vector<std::string> lines = {"aaa", "bbb"};
        const auto anchor = anchor_for("bbb", 2);
        const std::vector<HashlineEdit> edits = {
            {.op = HashlineEditOp::replace, .anchor = anchor, .content = {"XXX"}},
            {.op = HashlineEditOp::replace, .anchor = anchor, .content = {"XXX"}},
        };
        const auto result = apply_hashline_edits(lines, edits);
        expect(result.ok >> fatal);
        expect(result.lines[1] == "XXX");
    };
};
