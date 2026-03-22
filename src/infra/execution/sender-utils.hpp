#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <stdexec/execution.hpp>

namespace orangutan::execution {

template <stdexec::sender Sender>
auto sync_wait_or_throw(Sender &&sender, std::string_view label) {
    auto result = stdexec::sync_wait(std::forward<Sender>(sender));
    if (!result.has_value()) {
        throw std::runtime_error(std::string(label) + " stopped before producing a value");
    }
    return std::move(*result);
}

} // namespace orangutan::execution
