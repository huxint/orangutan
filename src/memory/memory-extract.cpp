#include "memory/memory-extract.hpp"
#include "infra/string.hpp"
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
        std::string extract_utf8_prefix(std::string_view input, std::size_t start, std::size_t max_codepoints, Stop should_stop) {
            if (start >= input.size() || max_codepoints == 0) {
                return {};
            }

            const auto valid_suffix = leading_valid_utf8_span(input.substr(start));
            if (valid_suffix.empty()) {
                return {};
            }

            auto view = una::ranges::utf8_view{valid_suffix};
            std::size_t consumed_bytes = 0;
            std::size_t count = 0;
            for (auto it = view.begin(); it != view.end() && count < max_codepoints; ++it) {
                const auto codepoint = *it;
                if (should_stop(codepoint)) {
                    break;
                }

                consumed_bytes = static_cast<std::size_t>(it.end() - valid_suffix.begin());
                ++count;
            }

            return std::string(valid_suffix.substr(0, consumed_bytes));
        }

        // Extract up to max_codepoints complete UTF-8 code points starting at byte offset,
        // stopping at ASCII whitespace or any of the given terminator code points.
        std::string extract_utf8_run(std::string_view input, std::size_t start, std::size_t max_codepoints, std::span<const char32_t> terminators) {
            return extract_utf8_prefix(input, start, max_codepoints, [&](char32_t codepoint) {
                return is_ascii_whitespace(codepoint) || std::ranges::contains(terminators, codepoint);
            });
        }

        // Like extract_utf8_run but allows spaces within (for sentence-level captures).
        // Stops at newline or any terminator.
        std::string extract_utf8_sentence(std::string_view input, std::size_t start, std::size_t max_codepoints, std::span<const char32_t> terminators) {
            return extract_utf8_prefix(input, start, max_codepoints, [&](char32_t codepoint) {
                return codepoint == U'\n' || std::ranges::contains(terminators, codepoint);
            });
        }

        std::size_t skip_optional_capture_prefix_separators(std::string_view input, std::size_t start) {
            const auto valid_suffix = leading_valid_utf8_span(input.substr(start));
            auto view = una::ranges::utf8_view{valid_suffix};
            std::size_t skipped_bytes = 0;
            for (auto it = view.begin(); it != view.end(); ++it) {
                const auto codepoint = *it;
                if (codepoint != U':' && codepoint != U'：' && codepoint != U' ') {
                    break;
                }

                skipped_bytes = static_cast<std::size_t>(it.end() - valid_suffix.begin());
            }

            return start + skipped_bytes;
        }

        enum class ChineseCaptureMode {
            Run,
            Sentence,
        };

        enum class ChineseKeyMode {
            Fixed,
            Hashed,
        };

        struct ChineseAutoCaptureRule {
            std::span<const std::string_view> prefixes;
            ChineseCaptureMode capture_mode;
            std::size_t max_codepoints;
            std::span<const char32_t> terminators;
            bool skip_optional_separators;
            ChineseKeyMode key_mode;
            std::string_view key;
            std::string_view category;
            base::f64 importance;
        };

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

        constexpr auto chinese_auto_capture_rules = std::to_array<ChineseAutoCaptureRule>({
            ChineseAutoCaptureRule{
                .prefixes = chinese_name_prefixes,
                .capture_mode = ChineseCaptureMode::Run,
                .max_codepoints = 20,
                .terminators = cjk_name_terminators,
                .skip_optional_separators = false,
                .key_mode = ChineseKeyMode::Fixed,
                .key = "profile.name",
                .category = "profile",
                .importance = 0.95,
            },
            ChineseAutoCaptureRule{
                .prefixes = chinese_project_prefixes,
                .capture_mode = ChineseCaptureMode::Sentence,
                .max_codepoints = 40,
                .terminators = cjk_sentence_terminators,
                .skip_optional_separators = false,
                .key_mode = ChineseKeyMode::Fixed,
                .key = "project.current",
                .category = "project",
                .importance = 0.8,
            },
            ChineseAutoCaptureRule{
                .prefixes = chinese_prefer_prefixes,
                .capture_mode = ChineseCaptureMode::Sentence,
                .max_codepoints = 40,
                .terminators = cjk_sentence_terminators,
                .skip_optional_separators = false,
                .key_mode = ChineseKeyMode::Fixed,
                .key = "preference.general",
                .category = "preference",
                .importance = 0.65,
            },
            ChineseAutoCaptureRule{
                .prefixes = chinese_remember_prefixes,
                .capture_mode = ChineseCaptureMode::Sentence,
                .max_codepoints = 60,
                .terminators = cjk_sentence_terminators,
                .skip_optional_separators = true,
                .key_mode = ChineseKeyMode::Hashed,
                .key = "fact.note.",
                .category = "fact",
                .importance = 0.85,
            },
        });

        std::string hash_key(std::string_view prefix, std::string_view value);

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

        std::string make_chinese_candidate_key(const ChineseAutoCaptureRule &rule, std::string_view content) {
            if (rule.key_mode == ChineseKeyMode::Hashed) {
                return hash_key(rule.key, content);
            }

            return std::string(rule.key);
        }

        std::string extract_chinese_candidate_content(std::string_view text, const ChineseAutoCaptureRule &rule, std::size_t start) {
            if (rule.skip_optional_separators) {
                start = skip_optional_capture_prefix_separators(text, start);
            }

            if (rule.capture_mode == ChineseCaptureMode::Run) {
                return extract_utf8_run(text, start, rule.max_codepoints, rule.terminators);
            }

            return extract_utf8_sentence(text, start, rule.max_codepoints, rule.terminators);
        }

        std::string hash_key(std::string_view prefix, std::string_view value) {
            return std::string(prefix) + std::to_string(std::hash<std::string_view>{}(value));
        }

        // Find the byte offset immediately after the first matching literal prefix, or npos.
        std::size_t find_after_any(std::string_view text, std::span<const std::string_view> prefixes) {
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
        const auto trimmed = utils::trim_copy(text);
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

    bool push_chinese_auto_candidate(std::vector<AutoCandidate> &candidates, std::string_view text, const ChineseAutoCaptureRule &rule) {
        auto pos = find_after_any(text, rule.prefixes);
        if (pos == std::string_view::npos) {
            return false;
        }

        const auto extracted = extract_chinese_candidate_content(text, rule, pos);
        const auto content = utils::trim_copy(extracted);
        if (content.empty()) {
            return false;
        }

        candidates.push_back({
            .key = make_chinese_candidate_key(rule, content),
            .content = static_cast<std::string>(content),
            .category = std::string(rule.category),
            .importance = rule.importance,
        });
        return true;
    }

    std::vector<AutoCandidate> extract_auto_candidates(const std::string &text) {
        const auto trimmed = utils::trim_copy(text);
        if (trimmed.empty()) {
            return {};
        }

        std::vector<AutoCandidate> candidates;
        candidates.reserve(5);

        const auto push_candidate = [&candidates](std::string key, std::string content, std::string category, base::f64 importance) {
            if (content.empty()) {
                return false;
            }

            candidates.push_back({
                .key = std::move(key),
                .content = std::move(content),
                .category = std::move(category),
                .importance = importance,
            });
            return true;
        };

        const auto push_ctre_capture = [&push_candidate](auto match_result, std::string key, std::string category, base::f64 importance) {
            if (!match_result || !match_result.template get<1>()) {
                return false;
            }

            auto content = utf8::sanitize(utils::trim_copy(match_result.template get<1>().to_view()));
            return push_candidate(std::move(key), std::move(content), std::move(category), importance);
        };

        const auto append_rule_candidate = [&](auto match_result, std::string key, std::string category, base::f64 importance, const ChineseAutoCaptureRule &fallback_rule) {
            if (push_ctre_capture(std::move(match_result), std::move(key), std::move(category), importance)) {
                return;
            }

            static_cast<void>(push_chinese_auto_candidate(candidates, trimmed, fallback_rule));
        };

        append_rule_candidate(ctre::search<R"((?i)\b(?:my name is|i am)\s+([^\s.!?\n][^.!?\n]{0,63}))">(trimmed), "profile.name", "profile", 0.95, chinese_auto_capture_rules[0]);

        append_rule_candidate(ctre::search<R"((?i)\b(?:we are working on|i(?:'m| am) working on|this project is about)\s+([^.!?\n]{1,120}))">(trimmed), "project.current",
                              "project", 0.8, chinese_auto_capture_rules[1]);

        if (auto match = ctre::search<R"((?i)\bmy favorite\s+([A-Za-z0-9 _\-]{1,32})\s+is\s+([^.!?\n]{1,120}))">(trimmed); match) {
            const auto aspect = utf8::sanitize(utils::trim_copy(match.get<1>().to_view()));
            const auto value = utf8::sanitize(utils::trim_copy(match.get<2>().to_view()));
            if (!aspect.empty()) {
                static_cast<void>(push_candidate("preference.favorite." + make_slug(aspect), value, "preference", 0.75));
            }
        }

        append_rule_candidate(ctre::search<R"((?i)\b(?:i prefer|i like)\s+([^.!?\n]{1,120}))">(trimmed), "preference.general", "preference", 0.65, chinese_auto_capture_rules[2]);

        if (auto match = ctre::search<R"((?i)\b(?:remember that|please remember)\s+([^.!?\n]{1,160}))">(trimmed); match) {
            const auto value = utf8::sanitize(utils::trim_copy(match.get<1>().to_view()));
            static_cast<void>(push_candidate(hash_key("fact.note.", value), value, "fact", 0.85));
        } else {
            static_cast<void>(push_chinese_auto_candidate(candidates, trimmed, chinese_auto_capture_rules[3]));
        }

        std::erase_if(candidates, [](const AutoCandidate &candidate) {
            return candidate.key.empty() || candidate.content.empty();
        });

        std::set<std::string> seen_keys;
        std::vector<AutoCandidate> deduped;
        deduped.reserve(candidates.size());
        for (auto &candidate : candidates) {
            if (seen_keys.emplace(candidate.key).second) {
                deduped.push_back(std::move(candidate));
            }
        }
        return deduped;
    }

    bool should_merge_auto_candidate(std::string_view key) {
        return key == "preference.general";
    }

} // namespace orangutan::memory_detail
