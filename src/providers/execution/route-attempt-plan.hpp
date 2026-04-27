#pragma once

#include "providers/provider.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::providers::execution {

    [[nodiscard]]
    std::string target_key(const ModelTarget &target);

    class RouteAttemptPlan {
    public:
        RouteAttemptPlan(const ProviderRoute &route, std::string_view preferred_target_key);

        [[nodiscard]]
        bool empty() const;

        [[nodiscard]]
        const ModelTarget &current() const;

        [[nodiscard]]
        bool can_advance_after(const ProviderError &error, bool emitted_stream_event) const;

        void advance();

        [[nodiscard]]
        std::size_t current_index() const;

        [[nodiscard]]
        std::span<const ModelTarget> targets() const;

    private:
        std::vector<ModelTarget> targets_;
        std::size_t current_index_ = 0;
    };

} // namespace orangutan::providers::execution
