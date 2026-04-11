#pragma once

#include <string>
#include <string_view>

namespace orangutan::utils {

    [[nodiscard]]
    inline std::string shell_single_quote_escape(std::string_view value) {
        std::string escaped{"'"};
        for (const auto ch : value) {
            if (ch == '\'') {
                escaped += "'\\''";
                continue;
            }
            escaped.push_back(ch);
        }
        escaped += '\'';
        return escaped;
    }

    [[nodiscard]]
    inline std::string escape_xml(std::string_view text) {
        std::string escaped;
        escaped.reserve(text.size());
        for (const char ch : text) {
            switch (ch) {
                case '&':
                    escaped += "&amp;";
                    break;
                case '<':
                    escaped += "&lt;";
                    break;
                case '>':
                    escaped += "&gt;";
                    break;
                case '"':
                    escaped += "&quot;";
                    break;
                case '\'':
                    escaped += "&apos;";
                    break;
                default:
                    escaped.push_back(ch);
                    break;
            }
        }
        return escaped;
    }

} // namespace orangutan::utils

namespace orangutan {

    using utils::escape_xml;
    using utils::shell_single_quote_escape;

} // namespace orangutan
