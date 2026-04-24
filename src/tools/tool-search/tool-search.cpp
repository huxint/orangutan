#include "tools/tool-search/tool-search.hpp"

#include "utils/format.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::tools {

    namespace {

        constexpr std::string_view SELECT_PREFIX = "select:";
        constexpr std::size_t DEFAULT_MAX_RESULTS = 5;

        std::string format_schema(const nlohmann::json &schema) {
            return schema.is_null() ? "{}" : schema.dump(2);
        }

        std::vector<std::string_view> query_terms(std::string_view query_lower) {
            std::vector<std::string_view> terms;
            for (std::size_t pos = 0; pos < query_lower.size();) {
                const auto first = query_lower.find_first_not_of(" \t\n\r\f\v,", pos);
                if (first == std::string_view::npos) {
                    break;
                }

                const auto last = query_lower.find_first_of(" \t\n\r\f\v,", first);
                const auto end = last == std::string_view::npos ? query_lower.size() : last;
                terms.push_back(query_lower.substr(first, end - first));
                pos = end;
            }
            return terms;
        }

        [[nodiscard]]
        bool has_synthetic_mcp_match(std::string_view tool_name, std::string_view term) {
            return term == "mcp" && tool_name.contains(':');
        }

        /// Score a summary against a lowercased query. Exact full-query match wins; otherwise accumulate per-term evidence.
        int score_summary(const DeferredToolSummary &summary, std::string_view query_lower, std::span<const std::string_view> terms) {
            const auto name = utils::ascii_to_lower_copy(summary.name);
            const auto description = utils::ascii_to_lower_copy(summary.description);

            if (name == query_lower) {
                return 100;
            }

            int score = 0;
            bool all_terms_in_name = true;
            for (const auto term : terms) {
                const bool in_name = name.contains(term);
                const bool in_description = description.contains(term);
                const bool synthetic_mcp_match = has_synthetic_mcp_match(name, term);
                const bool matched_in_name = in_name || synthetic_mcp_match;

                if (synthetic_mcp_match) {
                    score += 40;
                }
                if (matched_in_name) {
                    score += 15;
                    if (in_name && name == term) {
                        score += 15;
                    }
                } else {
                    all_terms_in_name = false;
                }
                if (in_description) {
                    score += 8;
                }
                if (!matched_in_name && !in_description) {
                    return 0;
                }
            }

            if (all_terms_in_name && !query_lower.empty()) {
                score += 20;
            }
            return score;
        }

        std::string render_tool(const ToolDef &def) {
            return fmt::format("## {}\n{}\n\nInput schema:\n```json\n{}\n```\n\n", def.name, def.description, format_schema(def.input_schema));
        }

        std::string select_tools(std::string_view names_csv, ToolRegistry &registry) {
            std::string out;
            std::size_t found{};
            for (const auto &name : utils::split_trimmed(names_csv)) {
                const auto *def = registry.find_definition(name);
                if (def == nullptr) {
                    utils::format_to(out, "Tool '{}' not found.\n\n", name);
                    continue;
                }
                registry.discover_tool(name);
                out += render_tool(*def);
                ++found;
            }
            utils::format_to(out, "Discovered {} tools. They are now available for use.", found);
            return out;
        }

        std::string list_summaries(std::string_view query, std::span<const DeferredToolSummary> summaries) {
            std::string out;
            utils::format_to(out, "No tools matching '{}'. Available deferred tools:\n", query);
            for (const auto &s : summaries) {
                utils::format_to(out, "- {}: {}\n", s.name, s.description);
            }
            return out;
        }

        std::string search_by_keyword(std::string_view query, std::size_t max_results, std::span<const DeferredToolSummary> summaries) {
            struct Match {
                const DeferredToolSummary *summary;
                int score;
            };

            const auto query_lower = utils::ascii_to_lower_copy(query);
            const auto terms = query_terms(query_lower);
            std::vector<Match> matches;
            matches.reserve(summaries.size());
            for (const auto &s : summaries) {
                if (const int score = score_summary(s, query_lower, terms); score > 0) {
                    matches.push_back({&s, score});
                }
            }

            if (matches.empty()) {
                return list_summaries(query, summaries);
            }

            const auto keep = std::min(matches.size(), max_results);
            std::ranges::partial_sort(matches, matches.begin() + static_cast<std::ptrdiff_t>(keep), std::ranges::greater{}, &Match::score);
            matches.resize(keep);

            std::string out;
            utils::format_to(out, "Found {} matching tools:\n\n", matches.size());
            for (const auto &m : matches) {
                utils::format_to(out, "- **{}**: {}\n", m.summary->name, m.summary->description);
            }
            out += "\nUse `select:<name>` to discover specific tools and get their full schemas.";
            return out;
        }

        std::string execute_tool_search(const nlohmann::json &input, ToolRegistry &registry) {
            const auto raw_query = input.value("query", std::string{});
            const auto query = std::string{utils::trim_copy(raw_query)};
            if (query.empty()) {
                return "error: query is required.";
            }

            if (query.starts_with(SELECT_PREFIX)) {
                return select_tools(std::string_view{query}.substr(SELECT_PREFIX.size()), registry);
            }

            const auto summaries = registry.deferred_tool_summaries();
            if (summaries.empty()) {
                return "No deferred tools available to search.";
            }

            const auto max_results = input.value<std::size_t>("max_results", DEFAULT_MAX_RESULTS);
            return search_by_keyword(query, max_results, summaries);
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
