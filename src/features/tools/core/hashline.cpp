#include "features/tools/core/hashline.hpp"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace orangutan {

constexpr std::string_view HASH_ALPHABET = "ZPMQVRWSNKTXJBYH";

namespace {

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

Anchor parse_anchor(std::string_view anchor_str) {
    if (anchor_str.empty()) {
        throw std::runtime_error("anchor string is empty");
    }

    const auto sep = anchor_str.find('#');
    if (sep == std::string_view::npos) {
        throw std::runtime_error("anchor missing '#' separator");
    }

    const auto line_part = anchor_str.substr(0, sep);
    const auto hash_part = anchor_str.substr(sep + 1);

    // Parse line number
    size_t line_number = 0;
    auto [ptr, ec] = std::from_chars(line_part.data(), line_part.data() + line_part.size(), line_number);
    if (ec != std::errc{} || ptr != line_part.data() + line_part.size()) {
        throw std::runtime_error("anchor line number is not a valid integer");
    }
    if (line_number == 0) {
        throw std::runtime_error("anchor line number must be >= 1");
    }

    // Validate hash: exactly 2 chars from HASH_ALPHABET
    if (hash_part.size() != 2) {
        throw std::runtime_error("anchor hash must be exactly 2 characters");
    }
    for (char ch : hash_part) {
        if (HASH_ALPHABET.find(ch) == std::string_view::npos) {
            throw std::runtime_error("anchor hash contains invalid character");
        }
    }

    return Anchor{.line = line_number, .hash = std::string(hash_part)};
}

std::optional<HashMismatch> validate_anchor(const Anchor &anchor,
                                             const std::vector<std::string> &lines) {
    if (anchor.line < 1 || anchor.line > lines.size()) {
        return HashMismatch{.line = anchor.line, .expected = anchor.hash, .actual = ""};
    }

    const auto &content = lines[anchor.line - 1];
    auto actual_hash = compute_line_hash(content, anchor.line);

    if (actual_hash != anchor.hash) {
        return HashMismatch{.line = anchor.line, .expected = anchor.hash, .actual = actual_hash};
    }

    return std::nullopt;
}

namespace {

// Check if a string matches the hashline prefix pattern: ^\d+#[ZPMQVRWSNKTXJBYH]{2}:(.*)
// Returns the position after the colon if matched, or std::nullopt.
std::optional<size_t> match_hashline_prefix(std::string_view s) {
    size_t pos = 0;
    // Must start with one or more digits
    if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) {
        return std::nullopt;
    }
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    // Must have '#'
    if (pos >= s.size() || s[pos] != '#') {
        return std::nullopt;
    }
    ++pos;
    // Must have exactly 2 chars from HASH_ALPHABET
    if (pos + 2 > s.size()) {
        return std::nullopt;
    }
    for (int i = 0; i < 2; ++i) {
        if (HASH_ALPHABET.find(s[pos + i]) == std::string_view::npos) {
            return std::nullopt;
        }
    }
    pos += 2;
    // Must have ':'
    if (pos >= s.size() || s[pos] != ':') {
        return std::nullopt;
    }
    ++pos;
    return pos;
}

// Internal resolved edit with parsed anchors
struct ResolvedEdit {
    size_t index;               // original index in edits vector
    HashlineEditOp op;
    std::optional<size_t> start_line; // 1-based, from anchor
    std::optional<size_t> end_line;   // 1-based, from end_anchor
    std::vector<std::string> content;
};

// Format context lines around a mismatch for error messages.
std::string format_mismatch_context(const std::vector<std::string> &lines,
                                     const HashMismatch &mm) {
    std::ostringstream oss;
    auto line_idx = mm.line - 1; // 0-based
    // 2 lines of context before
    for (size_t ctx = (line_idx >= 2 ? line_idx - 2 : 0); ctx < line_idx; ++ctx) {
        oss << "  " << format_hashline(lines[ctx], ctx + 1) << "\n";
    }
    // The mismatched line
    oss << ">>> " << format_hashline(lines[line_idx], line_idx + 1)
        << "  // <-- actual content\n";
    // 2 lines of context after
    for (size_t ctx = line_idx + 1; ctx < std::min(lines.size(), line_idx + 3); ++ctx) {
        oss << "  " << format_hashline(lines[ctx], ctx + 1) << "\n";
    }
    return oss.str();
}

// Effective range of a resolved edit (1-based, inclusive).
// For anchor-less inserts, returns {0,0} as they don't occupy a line range.
std::pair<size_t, size_t> effective_range(const ResolvedEdit &e) {
    if (!e.start_line.has_value()) {
        return {0, 0};
    }
    auto start = *e.start_line;
    auto end = e.end_line.value_or(start);
    return {start, end};
}

int edit_priority(HashlineEditOp op) {
    switch (op) {
    case HashlineEditOp::replace:
    case HashlineEditOp::del:
        return 2;
    case HashlineEditOp::insert_after:
        return 1;
    case HashlineEditOp::insert_before:
        return 0;
    }
    return 0;
}

} // namespace

HashlineEditResult apply_hashline_edits(const std::vector<std::string> &lines,
                                         const std::vector<HashlineEdit> &edits) {
    if (edits.empty()) {
        return {.ok = true, .lines = lines};
    }

    // Step 1: Per-op validation and anchor parsing
    std::vector<ResolvedEdit> resolved;
    resolved.reserve(edits.size());
    std::vector<HashMismatch> mismatches;

    for (size_t i = 0; i < edits.size(); ++i) {
        const auto &edit = edits[i];
        ResolvedEdit re{.index = i, .op = edit.op, .content = edit.content};

        // Validate required fields per op type
        switch (edit.op) {
        case HashlineEditOp::replace:
            if (edit.anchor.empty()) {
                return {.ok = false, .error = "replace edit requires anchor"};
            }
            break;
        case HashlineEditOp::insert_after:
            // anchor optional (no anchor = EOF append)
            break;
        case HashlineEditOp::insert_before:
            // anchor optional (no anchor = BOF prepend)
            break;
        case HashlineEditOp::del:
            if (edit.anchor.empty()) {
                return {.ok = false, .error = "delete edit requires anchor"};
            }
            break;
        }

        // Parse primary anchor
        if (!edit.anchor.empty()) {
            try {
                auto a = parse_anchor(edit.anchor);
                re.start_line = a.line;
                auto mm = validate_anchor(a, lines);
                if (mm.has_value()) {
                    mismatches.push_back(*mm);
                }
            } catch (const std::runtime_error &e) {
                return {.ok = false,
                        .error = "invalid anchor '" + edit.anchor + "': " + e.what()};
            }
        }

        // Parse end_anchor
        if (!edit.end_anchor.empty()) {
            try {
                auto a = parse_anchor(edit.end_anchor);
                re.end_line = a.line;
                auto mm = validate_anchor(a, lines);
                if (mm.has_value()) {
                    mismatches.push_back(*mm);
                }
            } catch (const std::runtime_error &e) {
                return {.ok = false,
                        .error = "invalid end_anchor '" + edit.end_anchor + "': " + e.what()};
            }
        }

        resolved.push_back(std::move(re));
    }

    // Step 3: Report all hash mismatches at once
    if (!mismatches.empty()) {
        std::ostringstream oss;
        for (const auto &mm : mismatches) {
            oss << "Hash mismatch at line " << mm.line
                << ": expected " << mm.expected << ", got "
                << (mm.actual.empty() ? "out-of-range" : mm.actual) << "\n";
            if (!mm.actual.empty()) {
                oss << format_mismatch_context(lines, mm);
            }
        }
        return {.ok = false, .error = oss.str()};
    }

    // Step: Validate ranges (end_anchor.line >= anchor.line)
    for (const auto &re : resolved) {
        if (re.start_line.has_value() && re.end_line.has_value()) {
            if (*re.end_line < *re.start_line) {
                return {.ok = false,
                        .error = "end_anchor line must be >= anchor line (got " +
                                 std::to_string(*re.end_line) + " <= " +
                                 std::to_string(*re.start_line) + ")"};
            }
        }
    }

    // Step 5: Deduplication -- identical edits collapsed, same target with different content = error
    // Build a key for each edit to detect duplicates/conflicts.
    // We compare (op, start_line, end_line, content).
    for (size_t i = 0; i < resolved.size(); ++i) {
        for (size_t j = i + 1; j < resolved.size(); ++j) {
            const auto &a = resolved[i];
            const auto &b = resolved[j];
            if (a.op == b.op && a.start_line == b.start_line && a.end_line == b.end_line) {
                if (a.content == b.content) {
                    // Identical -- mark j for removal (set start_line to a sentinel)
                    // We'll filter these out after this loop.
                    resolved[j].op = HashlineEditOp::replace; // any op
                    resolved[j].start_line = std::nullopt;
                    resolved[j].end_line = std::nullopt;
                    resolved[j].content.clear();
                    resolved[j].index = SIZE_MAX; // sentinel
                } else {
                    // Conflicting
                    auto line_num = a.start_line.value_or(0);
                    return {.ok = false,
                            .error = "conflicting edits at line " + std::to_string(line_num)};
                }
            }
        }
    }
    // Remove deduplicated entries
    std::erase_if(resolved, [](const ResolvedEdit &re) { return re.index == SIZE_MAX; });

    // Step 4: Detect overlapping ranges (only for replace/delete that occupy line ranges)
    for (size_t i = 0; i < resolved.size(); ++i) {
        if (resolved[i].op != HashlineEditOp::replace && resolved[i].op != HashlineEditOp::del) {
            continue;
        }
        auto [a_start, a_end] = effective_range(resolved[i]);
        if (a_start == 0) continue;
        for (size_t j = i + 1; j < resolved.size(); ++j) {
            if (resolved[j].op != HashlineEditOp::replace && resolved[j].op != HashlineEditOp::del) {
                continue;
            }
            auto [b_start, b_end] = effective_range(resolved[j]);
            if (b_start == 0) continue;
            // Check overlap: ranges [a_start, a_end] and [b_start, b_end]
            if (a_start <= b_end && b_start <= a_end) {
                return {.ok = false,
                        .error = "overlapping edits at lines " +
                                 std::to_string(a_start) + "-" + std::to_string(a_end) +
                                 " and " +
                                 std::to_string(b_start) + "-" + std::to_string(b_end)};
            }
        }
    }

    // Step 6: Content auto-stripping
    std::string warnings;
    for (auto &re : resolved) {
        if (re.content.empty()) continue;
        // Check if ALL non-empty content lines match the hashline prefix pattern
        bool all_match = true;
        for (const auto &line : re.content) {
            if (line.empty()) continue;
            if (!match_hashline_prefix(line).has_value()) {
                all_match = false;
                break;
            }
        }
        if (all_match) {
            // Check there is at least one non-empty line that matched
            bool has_non_empty = std::ranges::any_of(re.content, [](const std::string &s) {
                return !s.empty();
            });
            if (has_non_empty) {
                for (auto &line : re.content) {
                    if (line.empty()) continue;
                    auto pos = match_hashline_prefix(line);
                    if (pos.has_value()) {
                        line = line.substr(*pos);
                    }
                }
                if (warnings.empty()) {
                    warnings = "Warning: stripped hashline prefixes from content";
                }
            }
        }
    }

    // Step 7: Noop detection
    for (auto &re : resolved) {
        if (re.op != HashlineEditOp::replace) continue;
        if (!re.start_line.has_value()) continue;
        auto start = *re.start_line;
        auto end = re.end_line.value_or(start);
        // Check if content matches existing lines
        auto span_size = end - start + 1;
        if (re.content.size() == span_size) {
            bool same = true;
            for (size_t k = 0; k < span_size; ++k) {
                if (lines[start - 1 + k] != re.content[k]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                if (!warnings.empty()) warnings += "\n";
                warnings += "Noop: line " + std::to_string(start) +
                            " already has the specified content";
                re.index = SIZE_MAX; // mark for skip
            }
        }
    }
    // Remove noop edits
    std::erase_if(resolved, [](const ResolvedEdit &re) { return re.index == SIZE_MAX; });

    // Step 8: Sort edits bottom-up
    std::sort(resolved.begin(), resolved.end(), [](const ResolvedEdit &a, const ResolvedEdit &b) {
        // Anchor-less operations: EOF appends sort first (highest), BOF prepends sort last (lowest)
        bool a_has_anchor = a.start_line.has_value();
        bool b_has_anchor = b.start_line.has_value();

        if (!a_has_anchor && !b_has_anchor) {
            // Among anchor-less: EOF appends (insert_after) before BOF prepends (insert_before)
            if (a.op != b.op) {
                return a.op == HashlineEditOp::insert_after;
            }
            return false;
        }
        if (!a_has_anchor) {
            // a is anchor-less
            return a.op == HashlineEditOp::insert_after; // EOF sorts first, BOF sorts last
        }
        if (!b_has_anchor) {
            return b.op != HashlineEditOp::insert_after; // opposite logic
        }

        auto a_line = *a.start_line;
        auto b_line = *b.start_line;
        if (a_line != b_line) {
            return a_line > b_line; // higher lines first
        }
        // Same line: replace/delete > insert_after > insert_before
        return edit_priority(a.op) > edit_priority(b.op);
    });

    // Step 9: Apply edits bottom-up
    auto result_lines = lines;
    size_t edits_applied = 0;

    for (const auto &re : resolved) {
        switch (re.op) {
        case HashlineEditOp::replace: {
            auto start = *re.start_line; // 1-based
            auto end = re.end_line.value_or(start);
            auto begin_it = result_lines.begin() + static_cast<std::ptrdiff_t>(start - 1);
            auto end_it = result_lines.begin() + static_cast<std::ptrdiff_t>(end);
            result_lines.erase(begin_it, end_it);
            result_lines.insert(result_lines.begin() + static_cast<std::ptrdiff_t>(start - 1),
                                re.content.begin(), re.content.end());
            ++edits_applied;
            break;
        }
        case HashlineEditOp::insert_after: {
            if (re.start_line.has_value()) {
                auto pos = *re.start_line; // 1-based, insert after this line
                result_lines.insert(
                    result_lines.begin() + static_cast<std::ptrdiff_t>(pos),
                    re.content.begin(), re.content.end());
            } else {
                // No anchor -- append to end
                result_lines.insert(result_lines.end(), re.content.begin(), re.content.end());
            }
            ++edits_applied;
            break;
        }
        case HashlineEditOp::insert_before: {
            if (re.start_line.has_value()) {
                auto pos = *re.start_line; // 1-based, insert before this line
                result_lines.insert(
                    result_lines.begin() + static_cast<std::ptrdiff_t>(pos - 1),
                    re.content.begin(), re.content.end());
            } else {
                // No anchor -- prepend to beginning
                result_lines.insert(result_lines.begin(), re.content.begin(), re.content.end());
            }
            ++edits_applied;
            break;
        }
        case HashlineEditOp::del: {
            auto start = *re.start_line; // 1-based
            auto end = re.end_line.value_or(start);
            auto begin_it = result_lines.begin() + static_cast<std::ptrdiff_t>(start - 1);
            auto end_it = result_lines.begin() + static_cast<std::ptrdiff_t>(end);
            result_lines.erase(begin_it, end_it);
            ++edits_applied;
            break;
        }
        }
    }

    return {.ok = true,
            .warnings = warnings,
            .lines = std::move(result_lines),
            .edits_applied = edits_applied};
}

} // namespace orangutan
