#include "tools/tool-search/tool-search.hpp"

#include "utils/format.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstddef>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace orangutan::tools {

    namespace {
        constexpr std::string_view SELECT_PREFIX = "select:";

        std::string format_schema(const nlohmann::json &schema) {
            return schema.is_null() ? "{}" : schema.dump(2);
        }

        std::string execute_tool_search(const nlohmann::json &input, ToolRegistry &registry) {
            const auto query = utils::trim_copy(input.value("query", std::string{}));
            if (query.empty()) {
                return "error: query is required.";
            }

            auto append_tool_details = [&](std::string &out, std::string_view name) -> bool {
                const auto *def = registry.find_definition(static_cast<std::string>(name));
                if (def == nullptr) {
                    utils::format_to(out, "Tool '{}' not found.\n\n", name);
                    return false;
                }

                registry.discover_tool(static_cast<std::string>(name));
                utils::format_to(out, "## {}\n{}\n\nInput schema:\n```json\n{}\n```\n\n", def->name, def->description, format_schema(def->input_schema));
                return true;
            };

            // Direct selection: "select:tool1,tool2"
            if (query.starts_with(SELECT_PREFIX)) {
                std::string out;
                std::size_t found{};
                const auto select_query = query.substr(SELECT_PREFIX.size());

                for (const auto &name : utils::split_trimmed(select_query)) {
                    found += static_cast<std::size_t>(append_tool_details(out, name));
                }
                utils::format_to(out, "Discovered {} tool(s). They are now available for use.", found);
                return out;
            }

            // Keyword search: match against deferred tool names and
            // descriptions
            const auto summaries = registry.deferred_tool_summaries();
            if (summaries.empty()) {
                return "No deferred tools available to search.";
            }

            auto max_results = input.value<std::size_t>("max_results", 5);

            struct ScoredMatch {
                const DeferredToolSummary *summary = nullptr;
                int score = 0;
            };

            auto lower_copy = [](std::string_view sv) -> std::string {
                return sv | std::views::transform([](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       }) |
                       std::ranges::to<std::string>();
            };

            std::vector<ScoredMatch> matches;
            const auto query_lower = lower_copy(query);

            for (const auto &s : summaries) {
                int score = 0;
                const auto name_lower = lower_copy(s.name);
                const auto desc_lower = lower_copy(s.description);

                if (name_lower == query_lower) {
                    score += 100;
                } else if (name_lower.contains(query_lower)) {
                    score += 50;
                }

                if (desc_lower.contains(query_lower)) {
                    score += 20;
                }

                if (score > 0) {
                    matches.push_back({.summary = &s, .score = score});
                }
            }

            std::ranges::sort(matches, std::ranges::greater{}, &ScoredMatch::score);

            if (matches.size() > max_results) {
                matches.resize(static_cast<std::size_t>(max_results));
            }

            std::string out;

            if (matches.empty()) {
                utils::format_to(out, "No tools matching '{}'. Available deferred tools:\n", query);
                for (const auto &s : summaries) {
                    utils::format_to(out, "- {}: {}\n", s.name, s.description);
                }
            } else {
                utils::format_to(out, "Found {} matching tool(s):\n\n", matches.size());
                for (const auto &m : matches) {
                    utils::format_to(out, "- **{}**: {}\n", m.summary->name, m.summary->description);
                }
                utils::format_to(out, "\nUse `select:<name>` to discover specific tools and get their full schemas.");
            }
            return out;
        }

    } // namespace

    void register_tool_search(ToolRegistry &registry) {
        registry.register_tool({
            .definition =
                {
                    .name = "tool_search",
                    .description = "Search for and discover additional tools by name or keyword. "
                                   "Use 'select:name1,name2' to fetch specific tools, or keywords to search. "
                                   "Discovered tools become available for use in subsequent calls.",
                    .input_schema =
                        {
                            {"type", "object"},
                            {"properties",
                             {
                                 {"query", {{"type", "string"}, {"description", "Tool name ('select:tool1,tool2') or keyword search query"}}},
                                 {"max_results", {{"type", "integer"}, {"description", "Maximum results for keyword search (default 5)"}}},
                             }},
                            {"required", nlohmann::json::array({"query"})},
                        },
                },
            .execute =
                [&registry](const nlohmann::json &input) {
                    return execute_tool_search(input, registry);
                },
        });
    }

} // namespace orangutan::tools
