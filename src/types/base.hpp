#pragma once

#include <cstdint>

namespace orangutan::base {

    enum class role : std::uint8_t {
        user,
        assistant,
    };

    enum class origin : std::uint8_t {
        cli,
        channel,
        web,
    };

} // namespace orangutan::base
