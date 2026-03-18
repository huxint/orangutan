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
