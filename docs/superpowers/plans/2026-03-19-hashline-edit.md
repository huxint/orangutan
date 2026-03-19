# Hash-Anchored Edit Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a hash-anchored editing mode where file lines are identified by `LINE#HASH` tags, reducing token usage and improving edit reliability.

**Architecture:** Dual-mode system — both `hashline` and existing `search_replace` edit modes coexist, switchable via config (default: `hashline`). A new `hashline` module handles hash computation (xxHash32, 2-char encoding), anchor parsing/validation, and edit application. The `edit_mode` string is threaded through the registration chain from config → `register_runtime_tools` → `register_builtin_tools` → `register_builtin_core_tools` → `register_read_tool` / `register_edit_tool`.

**Tech Stack:** C++23, xxHash (header-only via FetchContent), GoogleTest, CMake

**Spec:** `docs/superpowers/specs/2026-03-19-hashline-edit-design.md`

---

### Task 1: Add xxHash dependency and create hashline module with core hashing

**Files:**
- Modify: `CMakeLists.txt` — add xxHash FetchContent, add `hashline.cpp` to sources
- Create: `src/features/tools/core/hashline.hpp` — header with hash types and function declarations
- Create: `src/features/tools/core/hashline.cpp` — implementation of `compute_line_hash` and `format_hashline`
- Create: `tests/features/hashline-test.cpp` — unit tests for hash computation
- Modify: `CMakeLists.txt` — add `tests/features/hashline-test.cpp` to test sources

- [ ] **Step 1: Write the failing test for hash computation**

Create `tests/features/hashline-test.cpp`:

```cpp
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
```

- [ ] **Step 2: Add xxHash FetchContent to CMakeLists.txt**

After the tomlplusplus FetchContent block (~line 26), add:

```cmake
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.3
    GIT_SHALLOW    TRUE
)
FetchContent_Populate(xxhash)
```

Add `src/features/tools/core/hashline.cpp` to the `orangutan_lib` sources list. Add `tests/features/hashline-test.cpp` to the `orangutan_tests` sources list.

- [ ] **Step 3: Create hashline.hpp**

Create `src/features/tools/core/hashline.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan {

// Compute 2-char hash for a line using xxHash32.
// Symbol-only lines (no alphanumeric chars) use line_number as seed;
// all other lines use seed=0.
std::string compute_line_hash(std::string_view line, size_t line_number);

// Format a line with hash tag: "LINE#HASH:content"
std::string format_hashline(std::string_view line, size_t line_number);

} // namespace orangutan
```

- [ ] **Step 4: Create hashline.cpp with compute_line_hash and format_hashline**

Create `src/features/tools/core/hashline.cpp`:

```cpp
#include "features/tools/core/hashline.hpp"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace orangutan {
namespace {

constexpr std::string_view HASH_ALPHABET = "ZPMQVRWSNKTXJBYH";

struct HashDict {
    std::array<std::array<char, 2>, 256> entries;

    consteval HashDict() : entries{} {
        for (size_t i = 0; i < 256; ++i) {
            entries[i][0] = HASH_ALPHABET[i >> 4];
            entries[i][1] = HASH_ALPHABET[i & 0xF];
        }
    }
};

constexpr HashDict HASH_DICT{};

bool is_symbol_only(std::string_view line) {
    return std::ranges::none_of(line, [](unsigned char ch) {
        return std::isalnum(ch);
    });
}

std::string_view preprocess_line(std::string_view line) {
    // Strip \r
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    // Trim trailing whitespace
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

} // namespace

std::string compute_line_hash(std::string_view line, size_t line_number) {
    const auto processed = preprocess_line(line);
    const auto seed = static_cast<XXH32_hash_t>(is_symbol_only(processed) ? line_number : 0);
    const auto hash = XXH32(processed.data(), processed.size(), seed) & 0xFF;
    const auto &entry = HASH_DICT.entries[hash];
    return {entry[0], entry[1]};
}

std::string format_hashline(std::string_view line, size_t line_number) {
    auto hash = compute_line_hash(line, line_number);
    return std::to_string(line_number) + "#" + hash + ":" + std::string(line);
}

} // namespace orangutan
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && cmake .. && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='Hashline*'`
Expected: All `HashlineTest` tests PASS

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/features/tools/core/hashline.hpp src/features/tools/core/hashline.cpp tests/features/hashline-test.cpp
git commit -m "feat: add hashline module with xxHash-based line hashing"
```

---

### Task 2: Add anchor parsing and validation to hashline module

**Files:**
- Modify: `src/features/tools/core/hashline.hpp` — add Anchor, HashMismatch structs and functions
- Modify: `src/features/tools/core/hashline.cpp` — implement parse_anchor and validate_anchor
- Modify: `tests/features/hashline-test.cpp` — add parsing/validation tests

- [ ] **Step 1: Write the failing tests for anchor parsing and validation**

Append to `tests/features/hashline-test.cpp`:

```cpp
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
        // Unlikely collision — skip this specific test case
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='Hashline*'`
Expected: New tests FAIL (parse_anchor and validate_anchor not declared)

- [ ] **Step 3: Add Anchor and HashMismatch structs and function declarations to hashline.hpp**

Add to `hashline.hpp` inside the `orangutan` namespace:

```cpp
struct Anchor {
    size_t line;
    std::string hash;
};

struct HashMismatch {
    size_t line;
    std::string expected;
    std::string actual;
};

// Parse anchor string "42#KQ" into {line_number, hash}.
// Throws std::runtime_error on invalid format.
Anchor parse_anchor(std::string_view anchor_str);

// Validate anchor against file contents.
// Returns mismatch info if hash doesn't match or line is out of range.
std::optional<HashMismatch> validate_anchor(const Anchor &anchor,
                                             const std::vector<std::string> &lines);
```

- [ ] **Step 4: Implement parse_anchor and validate_anchor in hashline.cpp**

```cpp
Anchor parse_anchor(std::string_view anchor_str) {
    if (anchor_str.empty()) {
        throw std::runtime_error("anchor is empty");
    }

    const auto hash_pos = anchor_str.find('#');
    if (hash_pos == std::string_view::npos) {
        throw std::runtime_error("invalid anchor format (missing #): " + std::string(anchor_str));
    }

    const auto line_str = anchor_str.substr(0, hash_pos);
    const auto hash_str = anchor_str.substr(hash_pos + 1);

    if (hash_str.size() != 2) {
        throw std::runtime_error("invalid anchor hash length: " + std::string(anchor_str));
    }

    for (char ch : hash_str) {
        if (HASH_ALPHABET.find(ch) == std::string_view::npos) {
            throw std::runtime_error("invalid anchor hash character: " + std::string(anchor_str));
        }
    }

    size_t line = 0;
    for (char ch : line_str) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid anchor line number: " + std::string(anchor_str));
        }
        line = line * 10 + static_cast<size_t>(ch - '0');
    }

    if (line == 0) {
        throw std::runtime_error("anchor line must be >= 1: " + std::string(anchor_str));
    }

    return {.line = line, .hash = std::string(hash_str)};
}

std::optional<HashMismatch> validate_anchor(const Anchor &anchor,
                                             const std::vector<std::string> &lines) {
    if (anchor.line > lines.size()) {
        return HashMismatch{
            .line = anchor.line,
            .expected = anchor.hash,
            .actual = "(out of range: file has " + std::to_string(lines.size()) + " lines)",
        };
    }

    const auto actual_hash = compute_line_hash(lines[anchor.line - 1], anchor.line);
    if (actual_hash != anchor.hash) {
        return HashMismatch{
            .line = anchor.line,
            .expected = anchor.hash,
            .actual = actual_hash,
        };
    }

    return std::nullopt;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='Hashline*'`
Expected: All tests PASS

- [ ] **Step 6: Commit**

```bash
git add src/features/tools/core/hashline.hpp src/features/tools/core/hashline.cpp tests/features/hashline-test.cpp
git commit -m "feat: add anchor parsing and validation to hashline module"
```

---

### Task 3: Add hashline edit application engine

**Files:**
- Modify: `src/features/tools/core/hashline.hpp` — add HashlineEdit struct and apply_hashline_edits declaration
- Modify: `src/features/tools/core/hashline.cpp` — implement edit application algorithm
- Modify: `tests/features/hashline-test.cpp` — add edit operation tests

- [ ] **Step 1: Write failing tests for edit operations**

Append to `tests/features/hashline-test.cpp`:

```cpp
#include <fstream>
#include <filesystem>

// Helper: build file lines and compute correct anchors
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
    // Error should include >>> marker on the mismatched line
    EXPECT_NE(result.error.find(">>>"), std::string::npos);
    // Error should include the actual hash for the mismatched line
    EXPECT_NE(result.error.find(actual_hash), std::string::npos);
    // Error should include surrounding context lines
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
    // When only SOME content lines have hash prefixes, do NOT strip
    std::vector<std::string> lines = {"aaa", "bbb"};
    std::vector<HashlineEdit> edits = {{
        .op = HashlineEditOp::replace,
        .anchor = anchor_for("bbb", 2),
        .content = {"10#KQ:line with prefix", "plain line without prefix"},
    }};
    auto result = apply_hashline_edits(lines, edits);
    ASSERT_TRUE(result.ok);
    // Content should NOT be stripped since not all non-empty lines match
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='HashlineEdit*'`
Expected: FAIL (HashlineEdit types not declared)

- [ ] **Step 3: Add edit types and apply_hashline_edits declaration to hashline.hpp**

```cpp
enum class HashlineEditOp {
    replace,
    insert_after,
    insert_before,
    del,
};

struct HashlineEdit {
    HashlineEditOp op;
    std::string anchor;       // "LINE#HASH" — optional for insert_after/insert_before
    std::string end_anchor;   // optional, for range operations
    std::vector<std::string> content;
};

struct HashlineEditResult {
    bool ok = false;
    std::string error;
    std::string warnings;
    std::vector<std::string> lines;
    size_t edits_applied = 0;
};

// Apply a sequence of hashline edits to a vector of lines.
// Returns the modified lines on success, or an error message on failure.
// All anchors are validated before any mutation (atomic pre-validation).
HashlineEditResult apply_hashline_edits(const std::vector<std::string> &lines,
                                         const std::vector<HashlineEdit> &edits);
```

- [ ] **Step 4a: Implement per-op field validation and anchor parsing/validation**

In `hashline.cpp`, implement the first half of `apply_hashline_edits`:
1. Per-op field validation — check each edit has the required fields for its `op` type (e.g., `replace` requires `anchor`, `insert_after` requires `content`). Return clear errors like `"replace requires 'anchor'"`.
2. Parse all anchors from edit operations. Anchor-less operations skip parsing.
3. Validate all anchors: for each anchor, check line is in range and hash matches. Collect **all** mismatches before failing. Format hash mismatch errors with 2-line context using `format_hashline`:
```
Hash mismatch at line 42: expected KQ, got PM
  40#VR:  int x = 1;
  41#WS:  int y = 2;
>>> 42#PM:  return x - y;  // <-- actual content
  43#NX:  }
```

See spec sections "Application Algorithm" steps 1-5 and "Error Handling".

- [ ] **Step 4b: Implement overlap detection, deduplication, and conflict checking**

Continue `apply_hashline_edits`:
4. Detect overlapping ranges: if two edits target overlapping `[anchor.line, end_anchor.line]` intervals, reject with `"overlapping edits at lines X-Y and A-B"`.
5. Deduplicate identical edits (same `op`, `anchor`, `end_anchor`, `content`). If two edits target the same anchor(s) with **different** content, error: `"conflicting edits at line N"`.

- [ ] **Step 4c: Implement content auto-stripping, noop detection, and sorting**

Continue `apply_hashline_edits`:
6. Content auto-stripping: if **all** non-empty content lines match `^\d+#[ZPMQVRWSNKTXJBYH]{2}:(.*)`, strip the prefix and add warning.
7. Noop detection: if a replace would produce identical content, warn and skip.
8. Sort edits bottom-up: highest effective line number first. Precedence at same line: replace/delete > insert_after > insert_before. Anchor-less: EOF appends sort first, BOF prepends sort last.

- [ ] **Step 4d: Implement the apply loop (replace, insert, delete operations)**

Apply edits via vector operations, working from bottom to top:
- `replace` single: erase line at `anchor.line-1`, insert `content` at that position
- `replace` range: erase from `anchor.line-1` to `end_anchor.line-1` (inclusive), insert `content`
- `insert_after`/`insert_before`: insert at correct position
- `delete` single/range: erase line(s)
- Count applied edits, return result

Note: map JSON `"delete"` string to `HashlineEditOp::del` (C++ reserves `delete` as keyword).

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='Hashline*'`
Expected: All tests PASS

- [ ] **Step 6: Commit**

```bash
git add src/features/tools/core/hashline.hpp src/features/tools/core/hashline.cpp tests/features/hashline-test.cpp
git commit -m "feat: add hashline edit application engine with validation"
```

---

### Task 4: Add edit_mode config field and CLI flag

**Files:**
- Modify: `src/infra/config/config.hpp:48` — add `edit_mode` field after `denied_tools`
- Modify: `src/infra/config/config.cpp:156-180` — parse `edit_mode` in `parse_tools_section()`
- Modify: `tests/infra/config-test.cpp` — add config parsing tests

- [ ] **Step 1: Write the failing test for config parsing**

Append to `tests/infra/config-test.cpp`:

```cpp
// ── edit_mode config ────────────────────────────

TEST(ConfigTest, DefaultEditModeIsHashline) {
    const Config cfg;
    EXPECT_EQ(cfg.edit_mode, "hashline");
}

TEST_F(ConfigFileTest, ParsesEditModeFromToolsSection) {
    auto path = write_config(R"toml(
[tools]
edit_mode = "search_replace"
)toml");
    auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.edit_mode, "search_replace");
}

TEST_F(ConfigFileTest, EditModeDefaultsToHashlineWhenNotSpecified) {
    auto path = write_config(R"toml(
[agent]
model = "test-model"
)toml");
    auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.edit_mode, "hashline");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='*EditMode*'`
Expected: FAIL (no `edit_mode` member)

- [ ] **Step 3: Add edit_mode field to Config struct**

In `src/infra/config/config.hpp`, after line 48 (`std::vector<std::string> denied_tools;`), add:

```cpp
std::string edit_mode = "hashline";  // "hashline" | "search_replace"
```

- [ ] **Step 4: Parse edit_mode in config.cpp**

In `src/infra/config/config.cpp`, inside `parse_tools_section()`, after the `denied` array parsing block (~line 177), add:

```cpp
if (auto mode = (*tools)["edit_mode"].value<std::string>()) {
    cfg.edit_mode = *mode;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='*EditMode*:*DefaultValues*'`
Expected: All PASS

- [ ] **Step 6: Commit**

```bash
git add src/infra/config/config.hpp src/infra/config/config.cpp tests/infra/config-test.cpp
git commit -m "feat: add edit_mode config field with hashline default"
```

---

### Task 5: Thread edit_mode through tool registration chain

The `edit_mode` default is `"search_replace"` in function signatures (not `"hashline"`) to avoid breaking existing callers. Only the top-level call site in `bootstrap.cpp` passes the config's `edit_mode` (which defaults to `"hashline"` in `Config`). This means: existing tests, `channel-serve.cpp`, and `subagent-manager.cpp` all get `"search_replace"` by default and continue working unchanged.

**Files:**
- Modify: `src/features/tools/core/internal.hpp:118,120` — update `register_read_tool` and `register_edit_tool` signatures
- Modify: `src/features/tools/core/register-core.cpp:8` — update `register_builtin_core_tools` signature, pass edit_mode
- Modify: `src/features/tools/builtin/register-builtin.hpp:10` — update `register_builtin_core_tools` signature
- Modify: `src/core/tools/tool.hpp:58` — update `register_builtin_tools` declaration (this is where it's declared, NOT in register-builtin.hpp)
- Modify: `src/features/tools/builtin/register-builtin.cpp:6` — pass edit_mode through
- Modify: `src/features/tools/runtime/runtime-loader.hpp:17` — update `register_runtime_tools` signature
- Modify: `src/features/tools/runtime/runtime-loader.cpp:37` — pass edit_mode through
- Modify: `src/features/tools/core/read.cpp:126` — accept edit_mode parameter (use it in Task 6)
- Modify: `src/features/tools/core/edit.cpp:246` — accept edit_mode parameter (use it in Task 7)

- [ ] **Step 1: Update register_read_tool and register_edit_tool signatures in internal.hpp**

Change lines 118-120 of `src/features/tools/core/internal.hpp`:

```cpp
void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root,
                        std::string_view edit_mode = "search_replace");
void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root,
                        std::string_view edit_mode = "search_replace");
```

- [ ] **Step 2: Update read.cpp and edit.cpp function signatures (accept but ignore edit_mode for now)**

In `src/features/tools/core/read.cpp`, change line 126:

```cpp
void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root,
                        std::string_view edit_mode) {
```

In `src/features/tools/core/edit.cpp`, change line 246:

```cpp
void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root,
                        std::string_view edit_mode) {
```

Both functions initially ignore `edit_mode` — the branching logic is added in Tasks 6 and 7.

- [ ] **Step 3: Update register_builtin_core_tools signature and implementation**

In `src/features/tools/core/register-core.cpp`, update the function signature:

```cpp
void register_builtin_core_tools(ToolRegistry &registry, const std::string &workspace,
                                  const ToolPermissionSettings *permissions,
                                  std::string_view edit_mode) {
```

Pass `edit_mode` to both calls:

```cpp
register_read_tool(registry, workspace_root, edit_mode);
register_edit_tool(registry, workspace_root, edit_mode);
```

- [ ] **Step 4: Update register_builtin_core_tools declaration in register-builtin.hpp**

In `src/features/tools/builtin/register-builtin.hpp` (line 10):

```cpp
void register_builtin_core_tools(ToolRegistry &registry, const std::string &workspace = {},
                                  const ToolPermissionSettings *permissions = nullptr,
                                  std::string_view edit_mode = "search_replace");
```

- [ ] **Step 5: Update register_builtin_tools declaration in tool.hpp**

**IMPORTANT:** `register_builtin_tools` is declared in `src/core/tools/tool.hpp:58`, NOT in `register-builtin.hpp`. Update line 58-59:

```cpp
void register_builtin_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory = nullptr,
                             const std::string &workspace = {},
                             const ToolRuntimeContext *tool_context = nullptr,
                             const ToolPermissionSettings *permissions = nullptr,
                             std::string_view edit_mode = "search_replace");
```

- [ ] **Step 6: Update register-builtin.cpp implementation to pass edit_mode**

```cpp
void register_builtin_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory, const std::string &workspace,
                             const ToolRuntimeContext *tool_context, const ToolPermissionSettings *permissions,
                             std::string_view edit_mode) {
    register_builtin_core_tools(registry, workspace, permissions, edit_mode);
    // ... rest unchanged ...
}
```

- [ ] **Step 7: Update runtime-loader.hpp and runtime-loader.cpp signatures**

In `runtime-loader.hpp`, add `std::string_view edit_mode = "search_replace"` to `register_runtime_tools`:

```cpp
RuntimeToolBootstrapResult register_runtime_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory,
                                                   const std::string &workspace,
                                                   const ToolRuntimeContext *tool_context,
                                                   const std::vector<Config::ScriptToolConfig> &custom_tools,
                                                   const std::vector<Config::McpServerConfig> &mcp_servers,
                                                   const ToolPermissionSettings *permissions = nullptr,
                                                   ToolApprovalCallback approval_callback = {},
                                                   std::string_view edit_mode = "search_replace");
```

In `runtime-loader.cpp`, update the implementation and pass `edit_mode` to `register_builtin_tools`.

- [ ] **Step 8: Pass edit_mode from bootstrap.cpp call site**

In `src/app/bootstrap.cpp` (~line 582), add `cfg.edit_mode` to the `register_runtime_tools` call:

```cpp
auto tool_bootstrap = orangutan::register_runtime_tools(
    tools, &runtime_memory, cli_identity.workspace, &tool_context,
    cfg.custom_tools, cfg.mcp_servers, &cfg.permissions, /*approval_callback=*/{},
    cfg.edit_mode);
```

Note: `channel-serve.cpp` (line 211) and `subagent-manager.cpp` (line 314) also call `register_runtime_tools` but use the default `"search_replace"` parameter. This is intentional — the config's `edit_mode` is only applied at the CLI entry point. These call sites compile unchanged.

- [ ] **Step 9: Build to verify compilation**

Run: `cd build && cmake .. && cmake --build . -j$(nproc)`
Expected: Build succeeds. All existing callers use the default `"search_replace"` and compile without changes.

- [ ] **Step 10: Run all tests to verify nothing is broken**

Run: `cd build && ./orangutan_tests`
Expected: All existing tests PASS (they all use the default `"search_replace"` mode since no test passes `edit_mode` explicitly)

- [ ] **Step 11: Commit**

```bash
git add src/features/tools/core/internal.hpp src/features/tools/core/register-core.cpp \
        src/features/tools/core/read.cpp src/features/tools/core/edit.cpp \
        src/features/tools/builtin/register-builtin.hpp src/features/tools/builtin/register-builtin.cpp \
        src/core/tools/tool.hpp \
        src/features/tools/runtime/runtime-loader.hpp src/features/tools/runtime/runtime-loader.cpp \
        src/app/bootstrap.cpp
git commit -m "refactor: thread edit_mode through tool registration chain"
```

---

### Task 6: Add hashline mode to read tool

**Files:**
- Modify: `src/features/tools/core/read.cpp` — add hashline-formatted output path
- Modify: `tests/core/tool-registry-test.cpp` — add read tool hashline mode tests

- [ ] **Step 1: Write failing tests for hashline read output**

Append to `tests/core/tool-registry-test.cpp` (or create a new test fixture that registers tools with `edit_mode = "hashline"`):

First, add a new test fixture that registers tools in hashline mode. After the `BuiltinToolsWorkspaceTest` fixture:

```cpp
class HashlineToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        workspace_ = test_tmp_root() / "orangutan_hashline_test";
        std::filesystem::remove_all(workspace_);
        std::filesystem::create_directories(workspace_);
        // Explicitly pass "hashline" — the default is "search_replace" to avoid breaking existing tests
        register_builtin_tools(registry_, nullptr, workspace_.string(), nullptr, nullptr, "hashline");
    }

    void TearDown() override {
        tmp_env_.reset();
        std::filesystem::remove_all(workspace_);
    }

    [[nodiscard]] const std::filesystem::path &workspace() const { return workspace_; }
    [[nodiscard]] ToolRegistry &registry() { return registry_; }

private:
    std::unique_ptr<ScopedEnvVar> tmp_env_;
    std::filesystem::path workspace_;
    ToolRegistry registry_;
};

TEST_F(HashlineToolsTest, ReadOutputHasHashTags) {
    std::ofstream(workspace() / "test.txt") << "hello world\nfoo bar\n";

    const auto result = registry().execute({.id = "r1", .name = "read", .input = {{"path", "test.txt"}}});
    EXPECT_FALSE(result.is_error);
    // Output should contain "1#XX:hello world" format
    EXPECT_NE(result.content.find("1#"), std::string::npos);
    EXPECT_NE(result.content.find(":hello world"), std::string::npos);
    EXPECT_NE(result.content.find("2#"), std::string::npos);
    EXPECT_NE(result.content.find(":foo bar"), std::string::npos);
}

TEST_F(HashlineToolsTest, ReadWithOffsetUsesOriginalLineNumbers) {
    std::ofstream(workspace() / "offset.txt") << "line1\nline2\nline3\nline4\n";

    const auto result = registry().execute({.id = "r2", .name = "read", .input = {{"path", "offset.txt"}, {"offset", 3}, {"limit", 1}}});
    EXPECT_FALSE(result.is_error);
    // Should use original line number 3, not display offset 1
    EXPECT_NE(result.content.find("3#"), std::string::npos);
    EXPECT_NE(result.content.find(":line3"), std::string::npos);
}

TEST_F(HashlineToolsTest, ReadMultiPathHasHashTags) {
    std::ofstream(workspace() / "a.txt") << "aaa\n";
    std::ofstream(workspace() / "b.txt") << "bbb\n";

    const auto result = registry().execute({.id = "r3", .name = "read", .input = {{"paths", json::array({"a.txt", "b.txt"})}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("=== "), std::string::npos);
    EXPECT_NE(result.content.find("1#"), std::string::npos);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='HashlineTools*'`
Expected: FAIL (read output is plain numbered, not hash-tagged)

- [ ] **Step 3: Modify read.cpp to support hashline output**

In `src/features/tools/core/read.cpp`:

1. Add `#include "features/tools/core/hashline.hpp"` at top
2. Change `read_single_file` to accept `std::string_view edit_mode` parameter
3. When `edit_mode == "hashline"`, output lines as `format_hashline(line, line_number)` instead of `setw(num_width) << i << '\t' << line`
4. Update the `read_file` function and lambda to capture and pass `edit_mode`

Key change in `read_single_file`:

```cpp
// Replace the line output loop:
for (int i = start; i <= end; ++i) {
    if (edit_mode == "hashline") {
        out << format_hashline(lines[static_cast<size_t>(i - 1)], static_cast<size_t>(i)) << '\n';
    } else {
        out << std::setw(num_width) << i << '\t' << lines[static_cast<size_t>(i - 1)] << '\n';
    }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='HashlineTools*:BuiltinToolsWorkspace*'`
Expected: Both hashline mode tests and existing search_replace mode tests PASS

- [ ] **Step 5: Commit**

```bash
git add src/features/tools/core/read.cpp tests/core/tool-registry-test.cpp
git commit -m "feat: add hashline mode to read tool output"
```

---

### Task 7: Add hashline mode to edit tool

**Files:**
- Modify: `src/features/tools/core/edit.cpp` — add hashline execution path
- Modify: `tests/core/tool-registry-test.cpp` — add hashline edit integration tests

- [ ] **Step 1: Write failing integration tests for hashline edit via registry**

Append to `tests/core/tool-registry-test.cpp`:

```cpp
TEST_F(HashlineToolsTest, EditReplaceSingleLine) {
    std::ofstream(workspace() / "target.cpp") << "int main() {\n    return 0;\n}\n";

    // First read to get hashes
    const auto read_result = registry().execute({.id = "r_edit", .name = "read", .input = {{"path", "target.cpp"}}});
    EXPECT_FALSE(read_result.is_error);

    // Get the anchor for line 2 ("    return 0;")
    auto hash = orangutan::compute_line_hash("    return 0;", 2);
    std::string anchor = "2#" + hash;

    json edits = json::array({{{"op", "replace"}, {"anchor", anchor}, {"content", json::array({"    return 42;"})}}});

    const auto result = registry().execute({.id = "e1", .name = "edit", .input = {{"path", "target.cpp"}, {"edits", edits}}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(result.content.find("Applied"), std::string::npos);

    std::ifstream ifs(workspace() / "target.cpp");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "int main() {\n    return 42;\n}\n");
}

TEST_F(HashlineToolsTest, EditDeleteLine) {
    std::ofstream(workspace() / "del.txt") << "aaa\nbbb\nccc\n";

    auto hash = orangutan::compute_line_hash("bbb", 2);
    json edits = json::array({{{"op", "delete"}, {"anchor", "2#" + hash}}});

    const auto result = registry().execute({.id = "e2", .name = "edit", .input = {{"path", "del.txt"}, {"edits", edits}}});
    EXPECT_FALSE(result.is_error);

    std::ifstream ifs(workspace() / "del.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "aaa\nccc\n");
}

TEST_F(HashlineToolsTest, EditInsertAfterEOF) {
    std::ofstream(workspace() / "ins.txt") << "aaa\nbbb\n";

    json edits = json::array({{{"op", "insert_after"}, {"content", json::array({"ccc"})}}});

    const auto result = registry().execute({.id = "e3", .name = "edit", .input = {{"path", "ins.txt"}, {"edits", edits}}});
    EXPECT_FALSE(result.is_error);

    std::ifstream ifs(workspace() / "ins.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "aaa\nbbb\nccc\n");
}

TEST_F(HashlineToolsTest, EditHashMismatchReturnsErrorWithContext) {
    std::ofstream(workspace() / "stale.txt") << "aaa\nbbb\nccc\n";

    json edits = json::array({{{"op", "replace"}, {"anchor", "2#ZZ"}, {"content", json::array({"XXX"})}}});

    auto actual_hash = orangutan::compute_line_hash("bbb", 2);
    if (actual_hash == "ZZ") {
        GTEST_SKIP() << "Hash collision";
    }

    const auto result = registry().execute({.id = "e4", .name = "edit", .input = {{"path", "stale.txt"}, {"edits", edits}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("mismatch"), std::string::npos);
    // Should contain correct hash in the error
    EXPECT_NE(result.content.find(actual_hash), std::string::npos);
}

TEST_F(HashlineToolsTest, EditContentAsStringIsSplitOnNewlines) {
    std::ofstream(workspace() / "str.txt") << "aaa\nbbb\n";

    auto hash = orangutan::compute_line_hash("bbb", 2);
    json edits = json::array({{{"op", "replace"}, {"anchor", "2#" + hash}, {"content", "line1\nline2"}}});

    const auto result = registry().execute({.id = "e5", .name = "edit", .input = {{"path", "str.txt"}, {"edits", edits}}});
    EXPECT_FALSE(result.is_error);

    std::ifstream ifs(workspace() / "str.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "aaa\nline1\nline2\n");
}

TEST_F(HashlineToolsTest, EditMissingAnchorForReplaceReturnsError) {
    std::ofstream(workspace() / "missing.txt") << "aaa\n";

    json edits = json::array({{{"op", "replace"}, {"content", json::array({"XXX"})}}});

    const auto result = registry().execute({.id = "e6", .name = "edit", .input = {{"path", "missing.txt"}, {"edits", edits}}});
    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("requires"), std::string::npos);
}

// ── Mode switching: schema differs between modes ──

TEST_F(HashlineToolsTest, EditToolDescriptionMentionsHashAnchors) {
    const auto defs = registry().definitions();
    for (const auto &def : defs) {
        if (def.name == "edit") {
            EXPECT_NE(def.description.find("hash"), std::string::npos);
            EXPECT_TRUE(def.input_schema.contains("properties"));
            EXPECT_TRUE(def.input_schema["properties"].contains("edits"));
            return;
        }
    }
    FAIL() << "edit tool not found in registry";
}

TEST(ModeSwitch, SearchReplaceEditToolDescriptionMentionsPatch) {
    ToolRegistry sr_registry;
    auto sr_workspace = orangutan::testing::test_tmp_root() / "orangutan_sr_mode_test";
    std::filesystem::create_directories(sr_workspace);
    register_builtin_tools(sr_registry, nullptr, sr_workspace.string(), nullptr, nullptr, "search_replace");

    const auto defs = sr_registry.definitions();
    for (const auto &def : defs) {
        if (def.name == "edit") {
            EXPECT_NE(def.description.find("patch"), std::string::npos);
            EXPECT_TRUE(def.input_schema.contains("properties"));
            EXPECT_TRUE(def.input_schema["properties"].contains("patch"));
            std::filesystem::remove_all(sr_workspace);
            return;
        }
    }
    std::filesystem::remove_all(sr_workspace);
    FAIL() << "edit tool not found in registry";
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='HashlineToolsTest.Edit*'`
Expected: FAIL (edit tool still uses patch format, doesn't accept `edits` field)

- [ ] **Step 3: Implement hashline execution path in edit.cpp**

In `src/features/tools/core/edit.cpp`:

1. Add `#include "features/tools/core/hashline.hpp"` at top
2. Add a new function `execute_hashline_edit(const json &input, const std::filesystem::path &workspace_root)`:
   - Extract `path` and `edits` array from JSON input
   - Resolve path via `resolve_tool_path()`
   - Read file into `std::vector<std::string>` lines
   - Convert JSON edits to `std::vector<HashlineEdit>`:
     - Map `"delete"` (JSON string) → `HashlineEditOp::del` (C++ `delete` is a keyword)
     - Map `"replace"` → `HashlineEditOp::replace`, etc.
     - Handle `content` as either string (split on `\n`) or array
   - Call `apply_hashline_edits(lines, edits)`
   - On error, throw `std::runtime_error(result.error)`
   - On success, write lines back to file and return summary with warnings
3. In `register_edit_tool`, branch on `edit_mode`:
   - When `"hashline"`: register with hashline schema, description, and `execute_hashline_edit` lambda
   - When `"search_replace"` (default / anything else): keep existing behavior

Since the default `edit_mode` in function signatures is `"search_replace"`, all existing callers (including tests) continue to get the search/replace behavior. No existing tests break.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd build && cmake --build . --target orangutan_tests -j$(nproc) && ./orangutan_tests --gtest_filter='HashlineToolsTest*:BuiltinToolsWorkspace*'`
Expected: All tests PASS (both hashline mode and existing search_replace mode)

- [ ] **Step 5: Run all tests to confirm backward compatibility**

Run: `cd build && ./orangutan_tests`
Expected: All tests PASS — both hashline mode tests (via `HashlineToolsTest` with explicit `"hashline"`) and all existing search_replace mode tests (via default `"search_replace"` parameter)

- [ ] **Step 6: Commit**

```bash
git add src/features/tools/core/edit.cpp tests/core/tool-registry-test.cpp
git commit -m "feat: add hashline execution path to edit tool"
```

---

### Task 8: Add CLI --edit-mode flag

Note: The spec says `src/app/main.cpp` but CLI parsing actually lives in `src/app/bootstrap.cpp` (`main.cpp` just delegates to `run_bootstrap`).

**Files:**
- Modify: `src/app/bootstrap.cpp` — add `--edit-mode` CLI option

- [ ] **Step 1: Find CLI option parsing in bootstrap.cpp**

Look for the `CLI::App` setup and `CliOptions` struct. Add `edit_mode` field to `CliOptions` and a CLI flag.

- [ ] **Step 2: Add --edit-mode flag**

In the `CliOptions` struct, add:

```cpp
std::string edit_mode;
```

In the CLI setup section, add:

```cpp
app.add_option("--edit-mode", options.edit_mode, "Edit tool mode: hashline or search_replace")
    ->default_val("")
    ->check(CLI::IsMember({"", "hashline", "search_replace"}));
```

After config loading, apply the override:

```cpp
if (!options.edit_mode.empty()) {
    cfg.edit_mode = options.edit_mode;
}
```

- [ ] **Step 3: Build and verify**

Run: `cd build && cmake --build . -j$(nproc)`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add src/app/bootstrap.cpp
git commit -m "feat: add --edit-mode CLI flag"
```

---

### Task 9: Verify full integration and run all tests

**Files:**
- No new files — verification task

- [ ] **Step 1: Run the complete test suite**

Run: `cd build && cmake .. && cmake --build . -j$(nproc) && ./orangutan_tests`
Expected: All tests PASS

- [ ] **Step 2: Manual smoke test (optional)**

Run: `cd build && ./orangutan --edit-mode=hashline --help`
Expected: `--edit-mode` appears in help output

Run: `cd build && ./orangutan --edit-mode=search_replace --help`
Expected: Accepted without error

- [ ] **Step 3: Run clang-format on new/modified files**

Run: `cd build && cmake --build . --target format`
Expected: Files formatted

- [ ] **Step 4: Final commit if format changes**

```bash
git add -u
git commit -m "style: format hashline implementation"
```
