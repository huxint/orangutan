#include "features/memory/memory-extract.hpp"
#include "features/memory/memory-search.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <regex>
#include <span>
#include <set>
#include <vector>

namespace orangutan::memory_detail {

std::string sanitize_utf8(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    size_t pos = 0;
    while (pos < input.size()) {
        const auto byte = static_cast<unsigned char>(input[pos]);

        size_t expected = 0;
        if (byte <= 0x7F) {
            expected = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            expected = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            expected = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            expected = 4;
        } else {
            ++pos;
            continue;
        }

        if (pos + expected > input.size()) {
            break;
        }

        bool valid = true;
        for (size_t i = 1; i < expected; ++i) {
            if ((static_cast<unsigned char>(input[pos + i]) & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }

        if (!valid) {
            ++pos;
            continue;
        }

        result.append(input.substr(pos, expected));
        pos += expected;
    }

    return result;
}

namespace {

// Returns byte length of the UTF-8 code point at input[pos], or 0 if invalid/incomplete.
size_t utf8_char_len(std::string_view input, size_t pos) {
    if (pos >= input.size()) {
        return 0;
    }
    const auto byte = static_cast<unsigned char>(input[pos]);
    size_t expected = 0;
    if (byte <= 0x7F) {
        expected = 1;
    } else if ((byte & 0xE0) == 0xC0) {
        expected = 2;
    } else if ((byte & 0xF0) == 0xE0) {
        expected = 3;
    } else if ((byte & 0xF8) == 0xF0) {
        expected = 4;
    } else {
        return 0;
    }
    if (pos + expected > input.size()) {
        return 0;
    }
    for (size_t i = 1; i < expected; ++i) {
        if ((static_cast<unsigned char>(input[pos + i]) & 0xC0) != 0x80) {
            return 0;
        }
    }
    return expected;
}

// Extract up to max_codepoints complete UTF-8 code points starting at byte offset,
// stopping at whitespace or any of the given terminator strings.
std::string extract_utf8_run(std::string_view input, size_t start, size_t max_codepoints, std::span<const std::string_view> terminators) {
    std::string result;
    size_t pos = start;
    size_t count = 0;

    while (pos < input.size() && count < max_codepoints) {
        const size_t char_len = utf8_char_len(input, pos);
        if (char_len == 0) {
            break;
        }

        const auto code_point = input.substr(pos, char_len);

        if (char_len == 1 && (std::isspace(static_cast<unsigned char>(code_point[0])) != 0)) {
            break;
        }

        bool is_terminator = false;
        for (const auto &term : terminators) {
            if (code_point == term) {
                is_terminator = true;
                break;
            }
        }
        if (is_terminator) {
            break;
        }

        result.append(code_point);
        pos += char_len;
        ++count;
    }

    return result;
}

// Like extract_utf8_run but allows spaces within (for sentence-level captures).
// Stops at newline or any terminator.
std::string extract_utf8_sentence(std::string_view input, size_t start, size_t max_codepoints, std::span<const std::string_view> terminators) {
    std::string result;
    size_t pos = start;
    size_t count = 0;

    while (pos < input.size() && count < max_codepoints) {
        const size_t char_len = utf8_char_len(input, pos);
        if (char_len == 0) {
            break;
        }

        const auto code_point = input.substr(pos, char_len);

        if (code_point == "\n") {
            break;
        }

        bool is_terminator = false;
        for (const auto &term : terminators) {
            if (code_point == term) {
                is_terminator = true;
                break;
            }
        }
        if (is_terminator) {
            break;
        }

        result.append(code_point);
        pos += char_len;
        ++count;
    }

    return result;
}

// CJK sentence terminators (as UTF-8 byte sequences).
constexpr auto cjk_name_terminators = std::to_array<std::string_view>({
    "\xe3\x80\x82", // 。
    "\xef\xbc\x81", // ！
    "\xef\xbc\x9f", // ？
    "\xef\xbc\x8c", // ，
});

constexpr auto cjk_sentence_terminators = std::to_array<std::string_view>({
    "\xe3\x80\x82", // 。
    "\xef\xbc\x81", // ！
    "\xef\xbc\x9f", // ？
});

std::string make_slug(std::string_view value) {
    std::string slug;
    slug.reserve(value.size());
    bool last_dash = false;

    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
            slug.push_back(static_cast<char>(std::tolower(ch)));
            last_dash = false;
            continue;
        }

        if (!last_dash) {
            slug.push_back('-');
            last_dash = true;
        }
    }

    while (!slug.empty() && slug.front() == '-') {
        slug.erase(slug.begin());
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }

    if (slug.empty()) {
        slug = std::to_string(std::hash<std::string_view>{}(value));
    }
    return slug;
}

std::string hash_key(std::string_view prefix, std::string_view value) {
    return std::string(prefix) + std::to_string(std::hash<std::string_view>{}(value));
}

// Find the byte offset immediately after a literal prefix, or npos.
size_t find_after(std::string_view text, std::string_view prefix) {
    const auto pos = text.find(prefix);
    if (pos == std::string_view::npos) {
        return std::string_view::npos;
    }
    return pos + prefix.size();
}

size_t find_after_any(std::string_view text, std::initializer_list<std::string_view> prefixes) {
    for (std::string_view prefix : prefixes) {
        const auto pos = find_after(text, prefix);
        if (pos != std::string_view::npos) {
            return pos;
        }
    }
    return std::string_view::npos;
}

} // namespace

bool should_attempt_auto_capture(const std::string &text) {
    const auto trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return false;
    }

    const auto normalized = normalize_ascii(trimmed);
    static constexpr auto exact_noise = std::to_array<std::string_view>({
        "hi",
        "hello",
        "hey",
        "thanks",
        "thank you",
        "ok",
        "okay",
        "sure",
        "sounds good",
        "got it",
        "great",
        "cool",
        "continue",
        "please continue",
    });
    if (std::ranges::contains(exact_noise, std::string_view{normalized})) {
        return false;
    }

    if (normalized.starts_with("stored memory [") || normalized.starts_with("updated memory [") || normalized.starts_with("forgot memory [") ||
        normalized.starts_with("[tool_result]") || normalized.starts_with("[tool_use]")) {
        return false;
    }

    const bool looks_question = trimmed.contains('?') || trimmed.contains("？");
    return !(looks_question &&
             (normalized.contains("remember") || normalized.contains("memory") || trimmed.contains("记住") || trimmed.contains("记得") || trimmed.contains("记忆")));
}
std::vector<AutoCandidate> extract_auto_candidates(const std::string &text) {
    std::vector<AutoCandidate> candidates;
    const auto trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return candidates;
    }

    // --- English patterns (ASCII-only character classes, safe with std::regex) ---
    const std::regex english_name(R"(\b(?:my name is|i am)\s+([^\s.!?\n][^.!?\n]{0,63}))", std::regex::icase);
    const std::regex english_project(R"(\b(?:we are working on|i(?:'m| am) working on|this project is about)\s+([^.!?\n]{1,120}))", std::regex::icase);
    const std::regex english_prefer(R"(\b(?:i prefer|i like)\s+([^.!?\n]{1,120}))", std::regex::icase);
    const std::regex english_favorite(R"(\bmy favorite\s+([A-Za-z0-9 _-]{1,32})\s+is\s+([^.!?\n]{1,120}))", std::regex::icase);
    const std::regex english_remember(R"(\b(?:remember that|please remember)\s+([^.!?\n]{1,160}))", std::regex::icase);

    const auto push_regex_match = [&candidates, &trimmed](const std::regex &pattern, std::string key, std::string category, double importance) {
        std::smatch local_match;
        if (!std::regex_search(trimmed, local_match, pattern) || local_match.size() < 2) {
            return false;
        }

        auto content = sanitize_utf8(trim_copy(local_match[1].str()));
        if (content.empty()) {
            return false;
        }
        candidates.push_back({.key = std::move(key), .content = std::move(content), .category = std::move(category), .importance = importance});
        return true;
    };

    // --- Chinese patterns: literal prefix detection + code-point-aware extraction ---

    const auto push_chinese_name = [&candidates, &trimmed]() {
        const auto pos = find_after_any(trimmed, {
                                                     "\xe6\x88\x91\xe5\x8f\xab", // 我叫
                                                     "\xe6\x88\x91\xe6\x98\xaf", // 我是
                                                 });
        if (pos == std::string_view::npos) {
            return false;
        }
        auto content = trim_copy(extract_utf8_run(trimmed, pos, 20, cjk_name_terminators));
        if (content.empty()) {
            return false;
        }
        candidates.push_back({.key = "profile.name", .content = std::move(content), .category = "profile", .importance = 0.95});
        return true;
    };

    const auto push_chinese_project = [&candidates, &trimmed]() {
        const auto pos = find_after_any(trimmed, {
                                                     "\xe6\x88\x91\xe4\xbb\xac\xe5\x9c\xa8\xe5\x81\x9a",             // 我们在做
                                                     "\xe5\xbd\x93\xe5\x89\x8d\xe9\xa1\xb9\xe7\x9b\xae\xe6\x98\xaf", // 当前项目是
                                                 });
        if (pos == std::string_view::npos) {
            return false;
        }
        auto content = trim_copy(extract_utf8_sentence(trimmed, pos, 40, cjk_sentence_terminators));
        if (content.empty()) {
            return false;
        }
        candidates.push_back({.key = "project.current", .content = std::move(content), .category = "project", .importance = 0.8});
        return true;
    };

    const auto push_chinese_prefer = [&candidates, &trimmed]() {
        const auto pos = find_after_any(trimmed, {
                                                     "\xe6\x88\x91\xe6\x9b\xb4\xe5\x96\x9c\xe6\xac\xa2", // 我更喜欢
                                                     "\xe6\x88\x91\xe5\x96\x9c\xe6\xac\xa2",             // 我喜欢
                                                 });
        if (pos == std::string_view::npos) {
            return false;
        }
        auto content = trim_copy(extract_utf8_sentence(trimmed, pos, 40, cjk_sentence_terminators));
        if (content.empty()) {
            return false;
        }
        candidates.push_back({.key = "preference.general", .content = std::move(content), .category = "preference", .importance = 0.65});
        return true;
    };

    const auto push_chinese_remember = [&candidates, &trimmed]() {
        auto pos = find_after_any(trimmed, {
                                               "\xe8\xaf\xb7\xe8\xae\xb0\xe4\xbd\x8f", // 请记住
                                               "\xe8\xae\xb0\xe4\xbd\x8f",             // 记住
                                           });
        if (pos == std::string_view::npos) {
            return false;
        }
        // Skip optional colon / fullwidth-colon / space after prefix.
        while (pos < trimmed.size()) {
            if (trimmed[pos] == ':' || trimmed[pos] == ' ') {
                ++pos;
                continue;
            }
            if (pos + 3 <= trimmed.size() && trimmed.substr(pos, 3) == "\xef\xbc\x9a") {
                pos += 3;
                continue;
            }
            break;
        }
        auto content = trim_copy(extract_utf8_sentence(trimmed, pos, 60, cjk_sentence_terminators));
        if (content.empty()) {
            return false;
        }
        candidates.push_back({.key = hash_key("fact.note.", content), .content = std::move(content), .category = "fact", .importance = 0.85});
        return true;
    };

    // --- Name ---
    std::smatch match;
    if (!push_regex_match(english_name, "profile.name", "profile", 0.95)) {
        static_cast<void>(push_chinese_name());
    }

    // --- Project ---
    if (!push_regex_match(english_project, "project.current", "project", 0.8)) {
        static_cast<void>(push_chinese_project());
    }

    // --- Favorite ---
    if (std::regex_search(trimmed, match, english_favorite) && match.size() >= 3) {
        const auto aspect = sanitize_utf8(trim_copy(match[1].str()));
        const auto value = sanitize_utf8(trim_copy(match[2].str()));
        if (!aspect.empty() && !value.empty()) {
            candidates.push_back({.key = "preference.favorite." + make_slug(aspect), .content = value, .category = "preference", .importance = 0.75});
        }
    }

    // --- Preference ---
    if (!push_regex_match(english_prefer, "preference.general", "preference", 0.65)) {
        static_cast<void>(push_chinese_prefer());
    }

    // --- Remember ---
    if (std::regex_search(trimmed, match, english_remember) && match.size() >= 2) {
        const auto value = sanitize_utf8(trim_copy(match[1].str()));
        if (!value.empty()) {
            candidates.push_back({.key = hash_key("fact.note.", value), .content = value, .category = "fact", .importance = 0.85});
        }
    } else {
        static_cast<void>(push_chinese_remember());
    }

    candidates.erase(std::ranges::remove_if(candidates,
                                            [](const AutoCandidate &candidate) {
                                                return candidate.key.empty() || candidate.content.empty();
                                            })
                         .begin(),
                     candidates.end());

    std::set<std::string> seen_keys;
    std::vector<AutoCandidate> deduped;
    deduped.reserve(candidates.size());
    for (auto &candidate : candidates) {
        if (seen_keys.insert(candidate.key).second) {
            deduped.push_back(std::move(candidate));
        }
    }
    return deduped;
}

bool should_merge_auto_candidate(std::string_view key) {
    return key == "preference.general";
}

} // namespace orangutan::memory_detail
