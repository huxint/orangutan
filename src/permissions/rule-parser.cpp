#include "permissions/rule-parser.hpp"

#include <algorithm>
#include <regex>
#include <string>

namespace orangutan::permissions {

    namespace {

        bool is_escaped(std::string_view str, size_t pos) {
            return pos > 0 && str[pos - 1] == '\\';
        }

        size_t find_first_unescaped(std::string_view str, char ch) {
            for (size_t i = 0; i < str.size(); ++i) {
                if (str[i] == ch && !is_escaped(str, i)) {
                    return i;
                }
            }
            return std::string_view::npos;
        }

        bool has_unescaped_wildcard(std::string_view str) {
            for (size_t i = 0; i < str.size(); ++i) {
                if (str[i] == '*' && !is_escaped(str, i)) {
                    return true;
                }
            }
            return false;
        }

        std::string unescape(std::string_view str) {
            std::string result;
            result.reserve(str.size());

            using namespace std::literals;

            for (std::size_t i = 0; i < str.size(); ++i) {
                if (str[i] == '\\' && i + 1 < str.size() && "()*"sv.contains(str[i + 1])) {
                    result.push_back(str[++i]);
                } else {
                    result.push_back(str[i]);
                }
            }

            return result;
        }

        RuleContent parse_content(std::string_view raw) {
            if (raw.size() >= 2 && raw.substr(raw.size() - 2) == ":*") {
                return {.match_type = rule_match_type::prefix, .pattern = unescape(raw.substr(0, raw.size() - 2))};
            }

            if (has_unescaped_wildcard(raw)) {
                return {.match_type = rule_match_type::wildcard, .pattern = std::string(raw)};
            }

            return {.match_type = rule_match_type::exact, .pattern = unescape(raw)};
        }

        bool icase_equal(std::string_view a, std::string_view b) {
            const auto lower = [](unsigned char ch) {
                return std::tolower(ch);
            };
            return std::ranges::equal(a, b, {}, lower, lower);
        }

        std::string wildcard_to_regex(std::string_view pattern) {
            constexpr std::string_view SPECIALS = R"(.+?^${}()|[]\)";
            std::string re;
            re.reserve(pattern.size() * 2);

            for (std::size_t i = 0, n = pattern.size(); i < n; ++i) {
                const char ch = pattern[i];
                if (ch == '\\' && i + 1 < n && pattern[i + 1] == '*') {
                    re += R"(\*)";
                    ++i;
                } else if (ch == '*') {
                    re += ".*";
                } else {
                    if (SPECIALS.contains(ch)) {
                        re.push_back('\\');
                    }
                    re.push_back(ch);
                }
            }

            return re;
        }

    } // namespace

    PermissionRule parse_permission_rule(std::string_view rule_str, permission_behavior behavior, permission_rule_source source) {
        auto paren_pos = find_first_unescaped(rule_str, '(');

        if (paren_pos == std::string_view::npos) {
            return {
                .source = source,
                .behavior = behavior,
                .tool_name = std::string(rule_str),
                .content = std::nullopt,
            };
        }

        auto tool_name = rule_str.substr(0, paren_pos);
        auto last_paren = rule_str.rfind(')');
        auto raw_content = rule_str.substr(paren_pos + 1, last_paren - paren_pos - 1);

        return {
            .source = source,
            .behavior = behavior,
            .tool_name = std::string(tool_name),
            .content = parse_content(raw_content),
        };
    }

    bool matches_prefix(std::string_view pattern, std::string_view input) {
        if (!input.starts_with(pattern)) {
            return false;
        }
        return input.size() == pattern.size() || input[pattern.size()] == ' ';
    }

    bool matches_wildcard(std::string_view pattern, std::string_view input) {
        auto re_str = wildcard_to_regex(pattern);
        std::regex re(re_str);
        return std::regex_match(input.begin(), input.end(), re);
    }

    bool matches_rule(const PermissionRule &rule, std::string_view tool_name, std::string_view content) {
        if (!icase_equal(rule.tool_name, tool_name)) {
            return false;
        }

        if (!rule.content) {
            return true;
        }

        const auto &rc = *rule.content;
        switch (rc.match_type) {
            case rule_match_type::exact:
                return rc.pattern == content;
            case rule_match_type::prefix:
                return matches_prefix(rc.pattern, content);
            case rule_match_type::wildcard:
                return matches_wildcard(rc.pattern, content);
        }
        return false;
    }

} // namespace orangutan::permissions
