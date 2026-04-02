#include "memory/memory-extract.hpp"
#include "memory/memory-type.hpp"
#include "utils/string.hpp"
#include "utils/utf8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctre.hpp>
#include <uni_algo/case.h>
#include <set>

namespace orangutan::memory::detail {

    namespace {

        std::string hash_key(std::string_view prefix, std::string_view value) {
            return std::string(prefix) + std::to_string(std::hash<std::string_view>{}(value));
        }

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

    std::vector<AutoCandidate> extract_auto_candidates(const std::string &text) {
        const auto trimmed = utils::trim_copy(text);
        if (trimmed.empty()) {
            return {};
        }

        std::vector<AutoCandidate> candidates;
        candidates.reserve(4);

        const auto push_candidate = [&candidates](std::string key, std::string content, std::string category, MemoryType type, base::f64 importance) {
            if (content.empty()) {
                return false;
            }
            candidates.push_back({
                .key = std::move(key),
                .content = std::move(content),
                .category = std::move(category),
                .type = type,
                .importance = importance,
            });
            return true;
        };

        // English patterns only — non-English extraction is handled by LLM distillation.
        if (auto match = ctre::search<R"((?i)\b(?:my name is|i am)\s+([^\s.!?\n][^.!?\n]{0,63}))">(trimmed); match) {
            auto content = utf8::sanitize(utils::trim_copy(match.get<1>().to_view()));
            static_cast<void>(push_candidate("profile.name", std::move(content), "profile", MemoryType::user, 0.95));
        }

        if (auto match = ctre::search<R"((?i)\b(?:we are working on|i(?:'m| am) working on|this project is about)\s+([^.!?\n]{1,120}))">(trimmed); match) {
            auto content = utf8::sanitize(utils::trim_copy(match.get<1>().to_view()));
            static_cast<void>(push_candidate("project.current", std::move(content), "project", MemoryType::project, 0.8));
        }

        if (auto match = ctre::search<R"((?i)\bmy favorite\s+([A-Za-z0-9 _\-]{1,32})\s+is\s+([^.!?\n]{1,120}))">(trimmed); match) {
            const auto aspect = utf8::sanitize(utils::trim_copy(match.get<1>().to_view()));
            const auto value = utf8::sanitize(utils::trim_copy(match.get<2>().to_view()));
            if (!aspect.empty()) {
                static_cast<void>(push_candidate("preference.favorite." + make_slug(aspect), value, "preference", MemoryType::user, 0.75));
            }
        }

        if (auto match = ctre::search<R"((?i)\b(?:i prefer|i like)\s+([^.!?\n]{1,120}))">(trimmed); match) {
            auto content = utf8::sanitize(utils::trim_copy(match.get<1>().to_view()));
            static_cast<void>(push_candidate("preference.general", std::move(content), "preference", MemoryType::user, 0.65));
        }

        if (auto match = ctre::search<R"((?i)\b(?:remember that|please remember)\s+([^.!?\n]{1,160}))">(trimmed); match) {
            const auto value = utf8::sanitize(utils::trim_copy(match.get<1>().to_view()));
            static_cast<void>(push_candidate(hash_key("fact.note.", value), value, "fact", MemoryType::reference, 0.85));
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

} // namespace orangutan::memory::detail
