#include "tools/tool-search/tool-search.hpp"

#include "utils/format.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>
#include <string_view>

namespace orangutan::tools {

    namespace {

        std::string to_lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char ch) {
                return std::tolower(ch);
            });
            return s;
        }

        std::string format_schema(const nlohmann::json &schema) {
            return schema.is_null() ? "{}" : schema.dump(2);
        }

        std::string execute_tool_search(const nlohmann::json &input, ToolRegistry &registry) {
            const auto query = input.value("query", "");
            if (query.empty()) {
                return "Error: query is required.";
            }

            // Direct selection: "select:tool1,tool2"
            if (query.starts_with("select:")) {
                const std::string_view names_sv = std::string_view(query).substr(7);
                std::vector<std::string> names;
                std::string_view remaining = names_sv;
                while (!remaining.empty()) {
                    auto comma = remaining.find(',');
                    auto token = remaining.substr(0, comma);
                    // Trim whitespace
                    auto start = token.find_first_not_of(" \t");
                    auto end = token.find_last_not_of(" \t");
                    if (start != std::string_view::npos) {
                        names.emplace_back(token.substr(start, end - start + 1));
                    }
                    remaining = (comma == std::string_view::npos) ? std::string_view{} : remaining.substr(comma + 1);
                }

                std::string out;
                int found = 0;
                for (const auto &n : names) {
                    const auto *def = registry.find_definition(n);
                    if (def == nullptr) {
                        utils::format_to(out, "Tool '{}' not found.\n\n", n);
                        continue;
                    }
                    registry.discover_tool(n);
                    utils::format_to(out, "## {}\n{}\n\nInput schema:\n```json\n{}\n```\n\n", def->name, def->description, format_schema(def->input_schema));
                    ++found;
                }
                utils::format_to(out, "Discovered {} tool(s). They are now available for use.", found);
                return out;
            }

            // Keyword search: match against deferred tool names and descriptions
            const auto summaries = registry.deferred_tool_summaries();
            if (summaries.empty()) {
                return "No deferred tools available to search.";
            }

            auto max_results = input.value("max_results", 5);

            struct ScoredMatch {
                const DeferredToolSummary *summary = nullptr;
                int score = 0;
            };

            std::vector<ScoredMatch> matches;
            const auto query_lower = to_lower(query);

            for (const auto &s : summaries) {
                int score = 0;
                const auto name_lower = to_lower(std::string(s.name));
                const auto desc_lower = to_lower(std::string(s.description));

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

            std::ranges::sort(matches, [](const ScoredMatch &a, const ScoredMatch &b) {
                return a.score > b.score;
            });
            if (std::cmp_greater(matches.size(), max_results)) {
                matches.resize(static_cast<std::size_t>(max_results));
            }

            if (matches.empty()) {
                std::string out = "No tools matching '" + query + "'. Available deferred tools:\n";
                for (const auto &s : summaries) {
                    utils::format_to(out, "- {}: {}\n", s.name, s.description);
                }
                return out;
            }

            std::string out;
            utils::format_to(out, "Found {} matching tool(s):\n\n", matches.size());
            for (const auto &m : matches) {
                utils::format_to(out, "- **{}**: {}\n", m.summary->name, m.summary->description);
            }
            out += "\nUse `select:<name>` to discover specific tools and get their full schemas.";
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
