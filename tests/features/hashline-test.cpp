#include "features/tools/core/hashline.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace orangutan;

// ── Hash computation ────────────────────────────

TEST(HashlineTest, ComputeLineHashReturns2CharString) {
    auto hash = compute_line_hash("int x = 42;", 1);
    EXPECT_EQ(hash.size(), 2);
}

TEST(HashlineTest, HashCharsAreFromAlphabet) {
    const std::string alphabet = "ZPMQVRWSNKTXJBYH";
    auto hash = compute_line_hash("return 0;", 1);
    EXPECT_NE(alphabet.find(hash[0]), std::string::npos);
    EXPECT_NE(alphabet.find(hash[1]), std::string::npos);
}

TEST(HashlineTest, SameContentSameSeedProducesSameHash) {
    auto h1 = compute_line_hash("int x = 1;", 1);
    auto h2 = compute_line_hash("int x = 1;", 2);
    // seed=0 for alphanumeric lines regardless of line_number
    EXPECT_EQ(h1, h2);
}

TEST(HashlineTest, SymbolOnlyLinesUseDifferentSeeds) {
    auto h1 = compute_line_hash("{", 1);
    auto h2 = compute_line_hash("{", 5);
    // symbol-only lines use line_number as seed, so different positions differ
    EXPECT_NE(h1, h2);
}

TEST(HashlineTest, EmptyLineIsSymbolOnly) {
    auto h1 = compute_line_hash("", 1);
    auto h2 = compute_line_hash("", 3);
    EXPECT_NE(h1, h2);
}

TEST(HashlineTest, TrailingWhitespaceDoesNotAffectHash) {
    auto h1 = compute_line_hash("int x;", 1);
    auto h2 = compute_line_hash("int x;   ", 1);
    EXPECT_EQ(h1, h2);
}

TEST(HashlineTest, CarriageReturnStrippedBeforeHash) {
    auto h1 = compute_line_hash("int x;", 1);
    auto h2 = compute_line_hash("int x;\r", 1);
    EXPECT_EQ(h1, h2);
}

// ── Format hashline ─────────────────────────────

TEST(HashlineTest, FormatHashlineProducesExpectedFormat) {
    auto line = format_hashline("int x = 1;", 5);
    // Should be "5#XX:int x = 1;" where XX is the 2-char hash
    EXPECT_TRUE(line.starts_with("5#"));
    EXPECT_NE(line.find(':'), std::string::npos);
    auto colon_pos = line.find(':');
    EXPECT_EQ(line.substr(colon_pos + 1), "int x = 1;");
}

TEST(HashlineTest, FormatHashlineHashPartIs2Chars) {
    auto line = format_hashline("return 0;", 10);
    // Format: "10#XX:return 0;"
    auto hash_start = line.find('#') + 1;
    auto colon_pos = line.find(':');
    EXPECT_EQ(colon_pos - hash_start, 2);
}

// ── Anchor parsing ──────────────────────────────

TEST(HashlineTest, ParseAnchorValidFormat) {
    auto anchor = parse_anchor("42#KQ");
    EXPECT_EQ(anchor.line, 42);
    EXPECT_EQ(anchor.hash, "KQ");
}

TEST(HashlineTest, ParseAnchorSingleDigitLine) {
    auto anchor = parse_anchor("1#ZZ");
    EXPECT_EQ(anchor.line, 1);
    EXPECT_EQ(anchor.hash, "ZZ");
}

TEST(HashlineTest, ParseAnchorThrowsOnMissingHash) {
    EXPECT_THROW(parse_anchor("42"), std::runtime_error);
}

TEST(HashlineTest, ParseAnchorThrowsOnNonNumericLine) {
    EXPECT_THROW(parse_anchor("abc#KQ"), std::runtime_error);
}

TEST(HashlineTest, ParseAnchorThrowsOnZeroLine) {
    EXPECT_THROW(parse_anchor("0#KQ"), std::runtime_error);
}

TEST(HashlineTest, ParseAnchorThrowsOnBadHashChars) {
    EXPECT_THROW(parse_anchor("5#AB"), std::runtime_error);
}

TEST(HashlineTest, ParseAnchorThrowsOnEmptyString) {
    EXPECT_THROW(parse_anchor(""), std::runtime_error);
}

// ── Anchor validation ───────────────────────────

TEST(HashlineTest, ValidateAnchorMatchingHash) {
    std::vector<std::string> lines = {"int x = 1;", "int y = 2;", "return x + y;"};
    auto expected_hash = compute_line_hash("int y = 2;", 2);
    Anchor anchor{.line = 2, .hash = expected_hash};
    auto mismatch = validate_anchor(anchor, lines);
    EXPECT_FALSE(mismatch.has_value());
}

TEST(HashlineTest, ValidateAnchorWrongHash) {
    std::vector<std::string> lines = {"int x = 1;", "int y = 2;"};
    Anchor anchor{.line = 2, .hash = "ZZ"};
    auto actual_hash = compute_line_hash("int y = 2;", 2);
    if (actual_hash == "ZZ") {
        GTEST_SKIP() << "Hash collision with ZZ";
    }
    auto mismatch = validate_anchor(anchor, lines);
    ASSERT_TRUE(mismatch.has_value());
    EXPECT_EQ(mismatch->line, 2);
    EXPECT_EQ(mismatch->expected, "ZZ");
    EXPECT_EQ(mismatch->actual, actual_hash);
}

TEST(HashlineTest, ValidateAnchorOutOfRange) {
    std::vector<std::string> lines = {"only one line"};
    Anchor anchor{.line = 5, .hash = "KQ"};
    auto mismatch = validate_anchor(anchor, lines);
    ASSERT_TRUE(mismatch.has_value());
    EXPECT_EQ(mismatch->line, 5);
}

// Helper: build anchor string for a line
static std::string anchor_for(const std::string &content, size_t line_number) {
    return std::to_string(line_number) + "#" + compute_line_hash(content, line_number);
}

// ── Edit operations ─────────────────────────────

TEST(HashlineEditTest, ReplaceSingleLine) {
    std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {"XXX"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.lines.size(), 3);
    EXPECT_EQ(result.lines[1], "XXX");
}

TEST(HashlineEditTest, ReplaceRange) {
    std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .end_anchor = anchor_for("ccc", 3),
        .content = {"NEW"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.lines.size(), 3);
    EXPECT_EQ(result.lines[0], "aaa");
    EXPECT_EQ(result.lines[1], "NEW");
    EXPECT_EQ(result.lines[2], "ddd");
}

TEST(HashlineEditTest, InsertAfterWithAnchor) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::insert_after,
        .anchor = anchor_for("aaa", 1),
        .content = {"INSERTED"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.lines.size(), 3);
    EXPECT_EQ(result.lines[1], "INSERTED");
}

TEST(HashlineEditTest, InsertAfterWithoutAnchorAppendsToEOF) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::insert_after,
        .content = {"APPENDED"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.lines.size(), 3);
    EXPECT_EQ(result.lines[2], "APPENDED");
}

TEST(HashlineEditTest, InsertBeforeWithAnchor) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::insert_before,
        .anchor = anchor_for("bbb", 2),
        .content = {"INSERTED"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.lines.size(), 3);
    EXPECT_EQ(result.lines[1], "INSERTED");
    EXPECT_EQ(result.lines[2], "bbb");
}

TEST(HashlineEditTest, InsertBeforeWithoutAnchorPrependsToBOF) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::insert_before,
        .content = {"PREPENDED"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.lines.size(), 3);
    EXPECT_EQ(result.lines[0], "PREPENDED");
}

TEST(HashlineEditTest, DeleteSingleLine) {
    std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::del,
        .anchor = anchor_for("bbb", 2),
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.lines.size(), 2);
    EXPECT_EQ(result.lines[0], "aaa");
    EXPECT_EQ(result.lines[1], "ccc");
}

TEST(HashlineEditTest, DeleteRange) {
    std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::del,
        .anchor = anchor_for("bbb", 2),
        .end_anchor = anchor_for("ccc", 3),
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.lines.size(), 2);
    EXPECT_EQ(result.lines[0], "aaa");
    EXPECT_EQ(result.lines[1], "ddd");
}

TEST(HashlineEditTest, MultipleEditsBottomUp) {
    std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
    std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .content = {"BBB"}},
        {.op = HashlineEditOp::replace, .anchor = anchor_for("ddd", 4), .content = {"DDD"}},
    };
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.lines[1], "BBB");
    EXPECT_EQ(result.lines[3], "DDD");
}

TEST(HashlineEditTest, ReplaceWithEmptyContentDeletesLine) {
    std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.lines.size(), 2);
    EXPECT_EQ(result.lines[0], "aaa");
    EXPECT_EQ(result.lines[1], "ccc");
}

// ── Error cases ─────────────────────────────────

TEST(HashlineEditTest, HashMismatchReturnsError) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = "2#ZZ",
        .content = {"XXX"},
    }};
    auto actual_hash = compute_line_hash("bbb", 2);
    if (actual_hash == "ZZ") {
        GTEST_SKIP() << "Hash collision with ZZ";
    }
    auto result = apply_hashline_edits(lines, edits);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("mismatch"), std::string::npos);
}

TEST(HashlineEditTest, HashMismatchErrorIncludesContextLines) {
    std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = "3#ZZ",
        .content = {"XXX"},
    }};
    auto actual_hash = compute_line_hash("ccc", 3);
    if (actual_hash == "ZZ") {
        GTEST_SKIP() << "Hash collision with ZZ";
    }
    auto result = apply_hashline_edits(lines, edits);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find(">>>"), std::string::npos);
    EXPECT_NE(result.error.find(actual_hash), std::string::npos);
    EXPECT_NE(result.error.find("bbb"), std::string::npos);
    EXPECT_NE(result.error.find("ddd"), std::string::npos);
}

TEST(HashlineEditTest, OverlappingEditsReturnsError) {
    std::vector<std::string> lines = {"aaa", "bbb", "ccc", "ddd", "eee"};
    std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = anchor_for("bbb", 2), .end_anchor = anchor_for("ddd", 4), .content = {"X"}},
        {.op = HashlineEditOp::del, .anchor = anchor_for("ccc", 3)},
    };
    auto result = apply_hashline_edits(lines, edits);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("overlapping"), std::string::npos);
}

TEST(HashlineEditTest, ConflictingEditsReturnsError) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    auto a = anchor_for("bbb", 2);
    std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = a, .content = {"XXX"}},
        {.op = HashlineEditOp::replace, .anchor = a, .content = {"YYY"}},
    };
    auto result = apply_hashline_edits(lines, edits);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("conflicting"), std::string::npos);
}

TEST(HashlineEditTest, InvalidRangeReturnsError) {
    std::vector<std::string> lines = {"aaa", "bbb", "ccc"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("ccc", 3),
        .end_anchor = anchor_for("aaa", 1),
        .content = {"X"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("<="), std::string::npos);
}

// ── Content auto-stripping ──────────────────────

TEST(HashlineEditTest, ContentAutoStrippingRemovesHashPrefixes) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {"10#KQ:replaced line"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.lines[1], "replaced line");
    EXPECT_NE(result.warnings.find("stripped"), std::string::npos);
}

TEST(HashlineEditTest, PartialHashPrefixDoesNotStrip) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {"10#KQ:line with prefix", "plain line without prefix"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.lines[1], "10#KQ:line with prefix");
    EXPECT_EQ(result.lines[2], "plain line without prefix");
}

// ── Noop detection ──────────────────────────────

TEST(HashlineEditTest, NoopReplaceSameContent) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {"bbb"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    EXPECT_NE(result.warnings.find("Noop"), std::string::npos);
}

// ── Deduplication ───────────────────────────────

TEST(HashlineEditTest, DuplicateIdenticalEditsAreDeduplicated) {
    std::vector<std::string> lines = {"aaa", "bbb"};
    auto a = anchor_for("bbb", 2);
    std::vector<HashlineEdit> edits = {
        {.op = HashlineEditOp::replace, .anchor = a, .content = {"XXX"}},
        {.op = HashlineEditOp::replace, .anchor = a, .content = {"XXX"}},
    };
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.lines[1], "XXX");
}
