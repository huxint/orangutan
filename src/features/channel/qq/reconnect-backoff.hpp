#pragma once

#include <array>
#include <chrono>
#include <cstddef>

namespace orangutan::qq {

class ReconnectBackoff {
public:
    [[nodiscard]]
    std::chrono::milliseconds next_delay() {
        const auto delay = delays.at(index_);
        if (index_ + 1 < delays.size()) {
            ++index_;
        }
        return delay;
    }

    void reset() {
        index_ = 0;
    }

private:
    static constexpr std::array delays{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(4), std::chrono::seconds(8), std::chrono::seconds(15),
    };
    std::size_t index_ = 0;
};

} // namespace orangutan::qq
