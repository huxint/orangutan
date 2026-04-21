#pragma once

#include "types/base.hpp"

namespace orangutan::tools::file {

    enum class edit_mode : std::uint8_t {
        search_replace,
        hashline,
    };

    inline constexpr edit_mode DEFAULT_EDIT_MODE = edit_mode::search_replace;

} // namespace orangutan::tools::file
