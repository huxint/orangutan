#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::prompt {

    enum class prompt_section_kind : std::uint8_t {
        static_section,
        dynamic_section,
        volatile_section,
    };

    enum class prompt_section_classification : std::uint8_t {
        must_keep,
        important,
        optional,
    };

    struct PromptSection {
        std::string id;
        prompt_section_kind kind = prompt_section_kind::static_section;
        prompt_section_classification classification = prompt_section_classification::optional;
        int priority = 0;
        std::string content;
        std::string cache_key_part;

        [[nodiscard]]
        std::size_t estimated_tokens() const {
            const auto bytes = static_cast<double>(content.size());
            return static_cast<std::size_t>(std::ceil(bytes / 4.0));
        }
    };

    struct PromptBuildInput {
        std::vector<PromptSection> sections;
        std::size_t token_budget = 0; // 0 means unlimited budget.
    };

    struct PromptBuildStats {
        std::size_t estimated_tokens = 0;
        bool budget_exceeded = false;
        std::string overflow_reason;
        std::vector<std::string> dropped_section_ids;
    };

    struct PromptBuildResult {
        std::string full_prompt;
        std::vector<PromptSection> emitted_sections;
        PromptBuildStats stats;
    };

    [[nodiscard]]
    inline PromptBuildResult compile_prompt(const PromptBuildInput &input) {
        auto sections = input.sections;
        std::ranges::sort(sections, [](const PromptSection &left, const PromptSection &right) {
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            return left.id < right.id;
        });

        PromptBuildResult result;
        if (sections.empty()) {
            return result;
        }

        const auto append_if_not_empty = [](std::string &out, std::string_view section) {
            if (section.empty()) {
                return;
            }
            if (!out.empty()) {
                out += "\n\n";
            }
            out.append(section);
        };

        const auto include_section = [&result, &append_if_not_empty](const PromptSection &section) {
            result.emitted_sections.push_back(section);
            append_if_not_empty(result.full_prompt, section.content);
            result.stats.estimated_tokens += section.estimated_tokens();
        };

        if (input.token_budget == 0) {
            for (const auto &section : sections) {
                include_section(section);
            }
            return result;
        }

        std::size_t must_keep_tokens = 0;
        for (const auto &section : sections) {
            if (section.classification == prompt_section_classification::must_keep) {
                must_keep_tokens += section.estimated_tokens();
            }
        }

        if (must_keep_tokens > input.token_budget) {
            for (const auto &section : sections) {
                if (section.classification == prompt_section_classification::must_keep) {
                    include_section(section);
                } else {
                    result.stats.dropped_section_ids.push_back(section.id);
                }
            }
            result.stats.budget_exceeded = true;
            result.stats.overflow_reason = "must_keep_floor";
            return result;
        }

        std::size_t used_tokens = 0;
        for (const auto &section : sections) {
            const auto section_tokens = section.estimated_tokens();
            if (section.classification == prompt_section_classification::must_keep) {
                include_section(section);
                used_tokens += section_tokens;
                continue;
            }

            if (used_tokens + section_tokens <= input.token_budget) {
                include_section(section);
                used_tokens += section_tokens;
            } else {
                result.stats.dropped_section_ids.push_back(section.id);
            }
        }

        if (!result.stats.dropped_section_ids.empty()) {
            result.stats.budget_exceeded = true;
        }
        return result;
    }

} // namespace orangutan::prompt
