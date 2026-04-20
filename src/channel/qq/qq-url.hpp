#pragma once

#include <string_view>

namespace orangutan::channel::qq {

    [[nodiscard]]
    inline bool is_absolute_url(std::string_view url) {
        return url.starts_with("https://") || url.starts_with("http://");
    }

} // namespace orangutan::channel::qq
