#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace orangutan::web {

    struct PageParams {
        std::size_t limit = 50;
        std::size_t offset = 0;
    };

    /// Parse `?limit=N&cursor=N` query params into a bounded page window.
    /// `cursor` is a simple integer offset (opaque to the client). Limits are clamped
    /// to [1, max_limit] with a sensible default so the frontend cannot accidentally
    /// DoS the server by requesting a million rows.
    [[nodiscard]]
    inline PageParams parse_page(const httplib::Request &req, std::size_t default_limit = 50, std::size_t max_limit = 500) {
        PageParams page{.limit = default_limit, .offset = 0};

        auto parse_number = [](std::string_view text, std::size_t fallback) -> std::size_t {
            std::size_t value = 0;
            const auto *begin = text.data();
            const auto *end = begin + text.size(); // NOLINT: pointer arithmetic bounded by text.size()
            const auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec != std::errc{} || ptr != end) {
                return fallback;
            }
            return value;
        };

        if (req.has_param("limit")) {
            page.limit = std::clamp(parse_number(req.get_param_value("limit"), default_limit), std::size_t{1}, max_limit);
        }
        if (req.has_param("cursor")) {
            page.offset = parse_number(req.get_param_value("cursor"), std::size_t{0});
        }
        return page;
    }

    /// Slice a contiguous collection and emit a pagination envelope:
    ///   {"items":[...], "page": {"limit": N, "cursor": N, "next": N|null, "total": N}}
    template <typename T, typename Serialize>
    [[nodiscard]]
    nlohmann::json paginate(std::span<const T> items, const PageParams &page, Serialize &&serialize) {
        const auto total = items.size();
        const auto start = std::min(page.offset, total);
        const auto end = std::min(start + page.limit, total);

        auto arr = nlohmann::json::array();
        for (std::size_t i = start; i < end; ++i) {
            arr.push_back(std::forward<Serialize>(serialize)(items[i]));
        }

        nlohmann::json page_info = {
            {"limit", page.limit},
            {"cursor", page.offset},
            {"total", total},
        };
        if (end < total) {
            page_info["next"] = end;
        } else {
            page_info["next"] = nullptr;
        }

        return nlohmann::json{
            {"items", std::move(arr)},
            {"page", std::move(page_info)},
        };
    }

} // namespace orangutan::web
