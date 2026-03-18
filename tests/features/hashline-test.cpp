#include "features/tools/core/hashline.hpp"

#include <gtest/gtest.h>
#include <string>

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
