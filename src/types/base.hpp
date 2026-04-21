#pragma once

#include <cstdint>

namespace orangutan::base {
    using f32 = float;
    using f64 = double;

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
