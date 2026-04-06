#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::channel {

    class Allowlist {
    public:
        Allowlist() = default;

        explicit Allowlist(std::vector<std::string> allow, std::vector<std::string> deny)
        : allow_(std::move(allow)),
          deny_(std::move(deny)) {}

        [[nodiscard]]
        bool is_allowed(std::string_view jid) const {
            if (std::ranges::any_of(deny_, [&jid](const auto &pattern) {
                    return matches(pattern, jid);
                })) {
                return false;
            }

            if (allow_.empty()) {
                return true;
            }

            return std::ranges::any_of(allow_, [&jid](const auto &pattern) {
                return matches(pattern, jid);
            });
        }

    private:
        std::vector<std::string> allow_;
        std::vector<std::string> deny_;

        static bool matches(std::string_view pattern, std::string_view jid) {
            if (pattern.empty()) {
                return jid.empty();
            }

            if (pattern.back() == '*') {
                pattern.remove_suffix(1);
                return jid.starts_with(pattern);
            }

            return pattern == jid;
        }
    };

} // namespace orangutan::channel

namespace orangutan {

    using channel::Allowlist;

} // namespace orangutan
