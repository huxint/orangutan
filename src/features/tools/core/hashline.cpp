#include "features/tools/core/hashline.hpp"

#include <rapidhash.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>

namespace orangutan {

constexpr std::string_view hash_alphabet = "ZPMQVRWSNKTXJBYH";

namespace {

struct HashDict {
    std::array<std::array<char, 2>, 256> entries;

    consteval HashDict()
    : entries{} {
        for (size_t i = 0; i < 256; ++i) {
            entries.at(i).at(0) = hash_alphabet.at(i >> 4);
            entries.at(i).at(1) = hash_alphabet.at(i & 0xF);
        }
    }
};

constexpr HashDict hash_dict{};

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

// Keep the rapidhash call isolated so hashline semantics live in compute_line_hash().
uint8_t hash_index_for_line(std::string_view normalized_line, uint64_t seed) {
    static constexpr char empty_line_sentinel = '\0';
    const void *data = normalized_line.empty() ? static_cast<const void *>(&empty_line_sentinel) : static_cast<const void *>(normalized_line.data());
    const auto hash = rapidhash_withSeed(data, normalized_line.size(), seed);
    return static_cast<uint8_t>(hash & 0xFFu);
}

} // namespace

std::string compute_line_hash(std::string_view line, size_t line_number) {
    const auto processed = preprocess_line(line);
    const auto seed = is_symbol_only(processed) ? static_cast<uint64_t>(line_number) : 0;
    const auto hash = hash_index_for_line(processed, seed);
    const auto &entry = hash_dict.entries.at(hash);
    return {entry.at(0), entry.at(1)};
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
    auto [ptr, ec] = std::from_chars(line_part.begin(), line_part.end(), line_number);
    if (ec != std::errc{} || ptr != line_part.end()) {
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
        if (!hash_alphabet.contains(ch)) {
            throw std::runtime_error("anchor hash contains invalid character");
        }
    }

    return Anchor{.line = line_number, .hash = std::string(hash_part)};
}

std::optional<HashMismatch> validate_anchor(const Anchor &anchor, const std::vector<std::string> &lines) {
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
    if (pos >= s.size() || std::isdigit(static_cast<unsigned char>(s[pos])) == 0) {
        return std::nullopt;
    }
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) != 0) {
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
    for (size_t i = 0; i < 2; ++i) {
        if (!hash_alphabet.contains(s.at(pos + i))) {
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
    size_t index; // original index in edits vector
    HashlineEditOp op;
    std::optional<size_t> start_line; // 1-based, from anchor
    std::optional<size_t> end_line;   // 1-based, from end_anchor
    std::vector<std::string> content;
};

struct RangeEditGroup {
    size_t start_line;
    size_t end_line;
    std::vector<std::string> before_content;
    std::vector<std::string> replacement_content;
    std::vector<std::string> after_content;
    size_t edit_count = 0;
    size_t order = 0;
};

struct AnchorInsertGroup {
    size_t line;
    std::vector<std::string> before_content;
    std::vector<std::string> after_content;
    size_t edit_count = 0;
    size_t order = 0;
};

enum class ApplyGroupKind {
    range,
    anchor,
    append,
    prepend,
};

struct ApplyGroup {
    ApplyGroupKind kind;
    size_t sort_line;
    size_t start_line = 0;
    size_t end_line = 0;
    std::vector<std::string> before_content;
    std::vector<std::string> replacement_content;
    std::vector<std::string> after_content;
    size_t edit_count = 0;
    size_t order = 0;
};

// Format context lines around a mismatch for error messages.
std::string format_mismatch_context(const std::vector<std::string> &lines, const HashMismatch &mm) {
    std::string out;
    auto line_idx = mm.line - 1; // 0-based
    // 2 lines of context before
    for (size_t ctx = (line_idx >= 2 ? line_idx - 2 : 0); ctx < line_idx; ++ctx) {
        out += "  ";
        out += format_hashline(lines[ctx], ctx + 1);
        out.push_back('\n');
    }
    // The mismatched line
    out += ">>> ";
    out += format_hashline(lines[line_idx], line_idx + 1);
    out += "  // <-- actual content\n";
    // 2 lines of context after
    for (size_t ctx = line_idx + 1; ctx < std::min(lines.size(), line_idx + 3); ++ctx) {
        out += "  ";
        out += format_hashline(lines[ctx], ctx + 1);
        out.push_back('\n');
    }
    return out;
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

bool is_range_edit(HashlineEditOp op) {
    return op == HashlineEditOp::replace || op == HashlineEditOp::del;
}

void append_content(std::vector<std::string> &target, const std::vector<std::string> &content) {
    target.insert(target.end(), content.begin(), content.end());
}

} // namespace

HashlineEditResult apply_hashline_edits(const std::vector<std::string> &lines, const std::vector<HashlineEdit> &edits) {
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
            case HashlineEditOp::insert_before:
            case HashlineEditOp::insert_after:
                // anchor optional (missing anchor inserts at BOF/EOF depending on op)
                break;
            case HashlineEditOp::del:
                if (edit.anchor.empty()) {
                    return {.ok = false, .error = "delete edit requires anchor"};
                }
                break;
            default:
                std::unreachable();
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
                return {.ok = false, .error = "invalid anchor '" + edit.anchor + "': " + e.what()};
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
                return {.ok = false, .error = "invalid end_anchor '" + edit.end_anchor + "': " + e.what()};
            }
        }

        resolved.push_back(std::move(re));
    }

    // Step 3: Report all hash mismatches at once
    if (!mismatches.empty()) {
        std::string error;
        for (const auto &mm : mismatches) {
            std::format_to(std::back_inserter(error), "Hash mismatch at line {}: expected {}, got {}\n", mm.line, mm.expected, mm.actual.empty() ? "out-of-range" : mm.actual);
            if (!mm.actual.empty()) {
                error += format_mismatch_context(lines, mm);
            }
        }
        return {.ok = false, .error = error};
    }

    // Step: Validate ranges (end_anchor.line >= anchor.line)
    for (const auto &re : resolved) {
        if (re.start_line.has_value() && re.end_line.has_value()) {
            if (*re.end_line < *re.start_line) {
                return {.ok = false, .error = "end_anchor line must be >= anchor line (got " + std::to_string(*re.end_line) + " <= " + std::to_string(*re.start_line) + ")"};
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
                    return {.ok = false, .error = "conflicting edits at line " + std::to_string(line_num)};
                }
            }
        }
    }
    // Remove deduplicated entries
    std::erase_if(resolved, [](const ResolvedEdit &re) {
        return re.index == SIZE_MAX;
    });

    // Step 4: Detect overlapping ranges (only for replace/delete that occupy line ranges)
    for (size_t i = 0; i < resolved.size(); ++i) {
        if (resolved[i].op != HashlineEditOp::replace && resolved[i].op != HashlineEditOp::del) {
            continue;
        }
        auto [a_start, a_end] = effective_range(resolved[i]);
        if (a_start == 0) {
            continue;
        }
        for (size_t j = i + 1; j < resolved.size(); ++j) {
            if (resolved[j].op != HashlineEditOp::replace && resolved[j].op != HashlineEditOp::del) {
                continue;
            }
            auto [b_start, b_end] = effective_range(resolved[j]);
            if (b_start == 0) {
                continue;
            }
            // Check overlap: ranges [a_start, a_end] and [b_start, b_end]
            if (a_start <= b_end && b_start <= a_end) {
                return {.ok = false,
                        .error = "overlapping edits at lines " + std::to_string(a_start) + "-" + std::to_string(a_end) + " and " + std::to_string(b_start) + "-" +
                                 std::to_string(b_end)};
            }
        }
    }

    // Step 6: Content auto-stripping
    std::string warnings;
    for (auto &re : resolved) {
        if (re.content.empty()) {
            continue;
        }
        // Check if ALL non-empty content lines match the hashline prefix pattern
        bool all_match = true;
        for (const auto &line : re.content) {
            if (line.empty()) {
                continue;
            }
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
                    if (line.empty()) {
                        continue;
                    }
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
        if (re.op != HashlineEditOp::replace) {
            continue;
        }
        if (!re.start_line.has_value()) {
            continue;
        }
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
                if (!warnings.empty()) {
                    warnings += "\n";
                }
                warnings += "Noop: line " + std::to_string(start) + " already has the specified content";
                re.index = SIZE_MAX; // mark for skip
            }
        }
    }
    // Remove noop edits
    std::erase_if(resolved, [](const ResolvedEdit &re) {
        return re.index == SIZE_MAX;
    });

    // Step 8: Group edits by the original lines they target so inserts stay attached to
    // the intended anchors even when a same-line replace/delete also fires in this batch.
    std::vector<RangeEditGroup> range_groups;
    std::vector<AnchorInsertGroup> anchor_groups;
    std::vector<ResolvedEdit> anchorless_edits;
    range_groups.reserve(resolved.size());
    anchor_groups.reserve(resolved.size());
    anchorless_edits.reserve(resolved.size());

    for (const auto &re : resolved) {
        if (!is_range_edit(re.op)) {
            continue;
        }

        auto start = *re.start_line;
        auto end = re.end_line.value_or(start);
        range_groups.push_back({
            .start_line = start,
            .end_line = end,
            .before_content = {},
            .replacement_content = re.op == HashlineEditOp::replace ? re.content : std::vector<std::string>{},
            .after_content = {},
            .edit_count = 1,
            .order = re.index,
        });
    }

    for (const auto &re : resolved) {
        if (is_range_edit(re.op)) {
            continue;
        }

        if (!re.start_line.has_value()) {
            anchorless_edits.push_back(re);
            continue;
        }

        const auto line = *re.start_line;
        bool attached_to_range = false;
        for (auto &group : range_groups) {
            if (re.op == HashlineEditOp::insert_before) {
                if (line == group.start_line) {
                    append_content(group.before_content, re.content);
                    ++group.edit_count;
                    group.order = std::min(group.order, re.index);
                    attached_to_range = true;
                    break;
                }
                if (group.start_line < line && line <= group.end_line) {
                    return {.ok = false,
                            .error = "insert_before at line " + std::to_string(line) + " targets a line inside edit range " + std::to_string(group.start_line) + "-" +
                                     std::to_string(group.end_line)};
                }
                continue;
            }

            if (line == group.end_line) {
                append_content(group.after_content, re.content);
                ++group.edit_count;
                group.order = std::min(group.order, re.index);
                attached_to_range = true;
                break;
            }
            if (group.start_line <= line && line < group.end_line) {
                return {.ok = false,
                        .error = "insert_after at line " + std::to_string(line) + " targets a line inside edit range " + std::to_string(group.start_line) + "-" +
                                 std::to_string(group.end_line)};
            }
        }
        if (attached_to_range) {
            continue;
        }

        auto it = std::ranges::find_if(anchor_groups, [line](const AnchorInsertGroup &group) {
            return group.line == line;
        });
        if (it == anchor_groups.end()) {
            anchor_groups.push_back({
                .line = line,
                .before_content = {},
                .after_content = {},
                .edit_count = 0,
                .order = re.index,
            });
            it = std::prev(anchor_groups.end());
        }

        if (re.op == HashlineEditOp::insert_before) {
            append_content(it->before_content, re.content);
        } else {
            append_content(it->after_content, re.content);
        }
        ++it->edit_count;
        it->order = std::min(it->order, re.index);
    }

    std::vector<ApplyGroup> apply_groups;
    apply_groups.reserve(range_groups.size() + anchor_groups.size() + anchorless_edits.size());
    for (auto &group : range_groups) {
        apply_groups.push_back({
            .kind = ApplyGroupKind::range,
            .sort_line = group.start_line,
            .start_line = group.start_line,
            .end_line = group.end_line,
            .before_content = std::move(group.before_content),
            .replacement_content = std::move(group.replacement_content),
            .after_content = std::move(group.after_content),
            .edit_count = group.edit_count,
            .order = group.order,
        });
    }
    for (auto &group : anchor_groups) {
        apply_groups.push_back({
            .kind = ApplyGroupKind::anchor,
            .sort_line = group.line,
            .start_line = group.line,
            .end_line = group.line,
            .before_content = std::move(group.before_content),
            .replacement_content = {},
            .after_content = std::move(group.after_content),
            .edit_count = group.edit_count,
            .order = group.order,
        });
    }
    for (const auto &re : anchorless_edits) {
        apply_groups.push_back({
            .kind = re.op == HashlineEditOp::insert_after ? ApplyGroupKind::append : ApplyGroupKind::prepend,
            .sort_line = re.op == HashlineEditOp::insert_after ? SIZE_MAX : 0,
            .start_line = 0,
            .end_line = 0,
            .before_content = {},
            .replacement_content = re.content,
            .after_content = {},
            .edit_count = 1,
            .order = re.index,
        });
    }

    std::ranges::stable_sort(apply_groups, [](const ApplyGroup &a, const ApplyGroup &b) {
        if (a.sort_line != b.sort_line) {
            return a.sort_line > b.sort_line;
        }
        return a.order < b.order;
    });

    // Step 9: Apply grouped edits bottom-up
    auto result_lines = lines;
    size_t edits_applied = 0;

    for (const auto &group : apply_groups) {
        switch (group.kind) {
            case ApplyGroupKind::range: {
                auto begin_it = result_lines.begin() + static_cast<std::ptrdiff_t>(group.start_line - 1);
                auto end_it = result_lines.begin() + static_cast<std::ptrdiff_t>(group.end_line);
                std::vector<std::string> combined;
                combined.reserve(group.before_content.size() + group.replacement_content.size() + group.after_content.size());
                append_content(combined, group.before_content);
                append_content(combined, group.replacement_content);
                append_content(combined, group.after_content);
                result_lines.erase(begin_it, end_it);
                result_lines.insert(result_lines.begin() + static_cast<std::ptrdiff_t>(group.start_line - 1), combined.begin(), combined.end());
                edits_applied += group.edit_count;
                break;
            }
            case ApplyGroupKind::anchor: {
                const auto line_index = static_cast<std::ptrdiff_t>(group.start_line - 1);
                result_lines.insert(result_lines.begin() + line_index + 1, group.after_content.begin(), group.after_content.end());
                result_lines.insert(result_lines.begin() + line_index, group.before_content.begin(), group.before_content.end());
                edits_applied += group.edit_count;
                break;
            }
            case ApplyGroupKind::append:
                result_lines.insert(result_lines.end(), group.replacement_content.begin(), group.replacement_content.end());
                edits_applied += group.edit_count;
                break;
            case ApplyGroupKind::prepend:
                result_lines.insert(result_lines.begin(), group.replacement_content.begin(), group.replacement_content.end());
                edits_applied += group.edit_count;
                break;
            default:
                std::unreachable();
        }
    }

    return {.ok = true, .warnings = warnings, .lines = std::move(result_lines), .edits_applied = edits_applied};
}

} // namespace orangutan
