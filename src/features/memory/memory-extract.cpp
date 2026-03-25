#include "features/memory/memory-extract.hpp"
#include "features/memory/memory-search.hpp"
#include "infra/utf8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <ctre.hpp>
#include <simdutf.h>
#include <span>
#include <uni_algo/case.h>
#include <uni_algo/ranges_conv.h>
#include <set>
#include <vector>

namespace orangutan::memory_detail {

namespace {

std::string_view leading_valid_utf8_span(std::string_view input) {
    if (input.empty()) {
        return input;
    }

    // Preserve the extraction helpers' old contract: stop at the first
    // malformed sequence instead of decoding past it as replacement chars.
    const auto validation = simdutf::validate_utf8_with_errors(input.data(), input.size());
    if (validation.error == simdutf::error_code::SUCCESS) {
        return input;
    }

    return input.substr(0, validation.count);
}

bool is_ascii_whitespace(char32_t codepoint) {
    return codepoint <= 0x7F && std::isspace(static_cast<unsigned char>(codepoint)) != 0;
}

template <typename Stop>
std::string extract_utf8_prefix(std::string_view input, size_t start, size_t max_codepoints, Stop &&should_stop) {
    if (start >= input.size() || max_codepoints == 0) {
        return {};
    }

    const auto valid_suffix = leading_valid_utf8_span(input.substr(start));
    if (valid_suffix.empty()) {
        return {};
    }

    auto view = una::ranges::utf8_view{valid_suffix};
    size_t consumed_bytes = 0;
    size_t count = 0;
    for (auto it = view.begin(); it != view.end() && count < max_codepoints; ++it) {
        const auto codepoint = *it;
        if (should_stop(codepoint)) {
            break;
        }

        consumed_bytes = static_cast<size_t>(it.end() - valid_suffix.begin());
        ++count;
    }

    return std::string(valid_suffix.substr(0, consumed_bytes));
}

// Extract up to max_codepoints complete UTF-8 code points starting at byte offset,
// stopping at ASCII whitespace or any of the given terminator code points.
std::string extract_utf8_run(std::string_view input, size_t start, size_t max_codepoints, std::span<const char32_t> terminators) {
    return extract_utf8_prefix(input, start, max_codepoints, [&](char32_t codepoint) {
        return is_ascii_whitespace(codepoint) || std::ranges::contains(terminators, codepoint);
    });
}

// Like extract_utf8_run but allows spaces within (for sentence-level captures).
// Stops at newline or any terminator.
std::string extract_utf8_sentence(std::string_view input, size_t start, size_t max_codepoints, std::span<const char32_t> terminators) {
    return extract_utf8_prefix(input, start, max_codepoints, [&](char32_t codepoint) {
        return codepoint == U'\n' || std::ranges::contains(terminators, codepoint);
    });
}

size_t skip_optional_capture_prefix_separators(std::string_view input, size_t start) {
    const auto valid_suffix = leading_valid_utf8_span(input.substr(start));
    auto view = una::ranges::utf8_view{valid_suffix};
    size_t skipped_bytes = 0;
    for (auto it = view.begin(); it != view.end(); ++it) {
        const auto codepoint = *it;
        if (codepoint != U':' && codepoint != U'：' && codepoint != U' ') {
            break;
        }

        skipped_bytes = static_cast<size_t>(it.end() - valid_suffix.begin());
    }

    return start + skipped_bytes;
}

// CJK punctuation/code-point terminators for literal-prefix captures.
constexpr auto cjk_name_terminators = std::to_array<char32_t>({
    U'。',
    U'！',
    U'？',
    U'，',
});

constexpr auto cjk_sentence_terminators = std::to_array<char32_t>({
    U'。',
    U'！',
    U'？',
});

constexpr auto chinese_name_prefixes = std::to_array<std::string_view>({
    "我叫",
    "我是",
});

constexpr auto chinese_project_prefixes = std::to_array<std::string_view>({
    "我们在做",
    "当前项目是",
});

constexpr auto chinese_prefer_prefixes = std::to_array<std::string_view>({
    "我更喜欢",
    "我喜欢",
});

constexpr auto chinese_remember_prefixes = std::to_array<std::string_view>({
    "请记住",
    "记住",
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

// Find the byte offset immediately after the first matching literal prefix, or npos.
size_t find_after_any(std::string_view text, std::span<const std::string_view> prefixes) {
    for (const auto prefix : prefixes) {
        const auto pos = text.find(prefix);
        if (pos != std::string_view::npos) {
            return pos + prefix.size();
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

    const auto normalized = una::cases::to_lowercase_utf8(trimmed);
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

    // --- English patterns (compile-time via CTRE) ---

    const auto push_ctre_match = [&candidates](auto match_result, std::string key, std::string category, double importance) {
        if (!match_result || !match_result.template get<1>()) {
            return false;
        }
        auto content = utf8::sanitize(trim_copy(std::string(match_result.template get<1>().to_view())));
        if (content.empty()) {
            return false;
        }
        candidates.push_back({.key = std::move(key), .content = std::move(content), .category = std::move(category), .importance = importance});
        return true;
    };

    // --- Chinese patterns: literal prefix detection + code-point-aware extraction ---

    const auto push_chinese_name = [&candidates, &trimmed]() {
        const auto pos = find_after_any(trimmed, chinese_name_prefixes);
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
        const auto pos = find_after_any(trimmed, chinese_project_prefixes);
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
        const auto pos = find_after_any(trimmed, chinese_prefer_prefixes);
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
        auto pos = find_after_any(trimmed, chinese_remember_prefixes);
        if (pos == std::string_view::npos) {
            return false;
        }
        pos = skip_optional_capture_prefix_separators(trimmed, pos);
        auto content = trim_copy(extract_utf8_sentence(trimmed, pos, 60, cjk_sentence_terminators));
        if (content.empty()) {
            return false;
        }
        candidates.push_back({.key = hash_key("fact.note.", content), .content = std::move(content), .category = "fact", .importance = 0.85});
        return true;
    };

    // --- Name ---
    if (!push_ctre_match(ctre::search<R"((?i)\b(?:my name is|i am)\s+([^\s.!?\n][^.!?\n]{0,63}))">(trimmed), "profile.name", "profile", 0.95)) {
        static_cast<void>(push_chinese_name());
    }

    // --- Project ---
    if (!push_ctre_match(ctre::search<R"((?i)\b(?:we are working on|i(?:'m| am) working on|this project is about)\s+([^.!?\n]{1,120}))">(trimmed), "project.current", "project",
                         0.8)) {
        static_cast<void>(push_chinese_project());
    }

    // --- Favorite ---
    if (auto fm = ctre::search<R"((?i)\bmy favorite\s+([A-Za-z0-9 _\-]{1,32})\s+is\s+([^.!?\n]{1,120}))">(trimmed); fm) {
        const auto aspect = utf8::sanitize(trim_copy(std::string(fm.get<1>().to_view())));
        const auto value = utf8::sanitize(trim_copy(std::string(fm.get<2>().to_view())));
        if (!aspect.empty() && !value.empty()) {
            candidates.push_back({.key = "preference.favorite." + make_slug(aspect), .content = value, .category = "preference", .importance = 0.75});
        }
    }

    // --- Preference ---
    if (!push_ctre_match(ctre::search<R"((?i)\b(?:i prefer|i like)\s+([^.!?\n]{1,120}))">(trimmed), "preference.general", "preference", 0.65)) {
        static_cast<void>(push_chinese_prefer());
    }

    // --- Remember ---
    if (auto rm = ctre::search<R"((?i)\b(?:remember that|please remember)\s+([^.!?\n]{1,160}))">(trimmed); rm) {
        const auto value = utf8::sanitize(trim_copy(std::string(rm.get<1>().to_view())));
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
