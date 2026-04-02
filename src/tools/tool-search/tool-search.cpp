#include "tools/tool-search/tool-search.hpp"

#include "utils/format.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace orangutan::tools {

    namespace {

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
                const auto names_str = query.substr(7);
                std::vector<std::string> names;
                std::istringstream stream(names_str);
                std::string name;
                while (std::getline(stream, name, ',')) {
                    auto trimmed = name;
                    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
                    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
                    if (!trimmed.empty()) {
                        names.push_back(std::move(trimmed));
                    }
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
            const auto query_lower = [&query]() {
                std::string lower = query;
                std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
                    return std::tolower(ch);
                });
                return lower;
            }();

            for (const auto &s : summaries) {
                int score = 0;
                auto name_lower = s.name;
                std::ranges::transform(name_lower, name_lower.begin(), [](unsigned char ch) {
                    return std::tolower(ch);
                });
                auto desc_lower = s.description;
                std::ranges::transform(desc_lower, desc_lower.begin(), [](unsigned char ch) {
                    return std::tolower(ch);
                });

                if (name_lower == query_lower) {
                    score += 100;
                } else if (name_lower.contains(query_lower)) {
                    score += 50;
                }
                if (desc_lower.contains(query_lower)) {
                    score += 20;
                }

                // Also check individual words in query
                std::istringstream words(query_lower);
                std::string word;
                while (words >> word) {
                    if (name_lower.contains(word)) {
                        score += 10;
                    }
                    if (desc_lower.contains(word)) {
                        score += 5;
                    }
                }

                if (score > 0) {
                    matches.push_back({.summary = &s, .score = score});
                }
            }

            std::ranges::sort(matches, [](const ScoredMatch &a, const ScoredMatch &b) {
                return a.score > b.score;
            });
            if (static_cast<int>(matches.size()) > max_results) {
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
            for (const auto &m : matches) {
                const auto *def = registry.find_definition(m.summary->name);
                if (def == nullptr) {
                    continue;
                }
                registry.discover_tool(m.summary->name);
                utils::format_to(out, "## {}\n{}\n\nInput schema:\n```json\n{}\n```\n\n", def->name, def->description, format_schema(def->input_schema));
            }
            utils::format_to(out, "Discovered {} tool(s). They are now available for use.", matches.size());
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
