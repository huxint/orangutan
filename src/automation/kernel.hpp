#pragma once

#include "automation/store.hpp"
#include "utils/transparent-lookup.hpp"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::automation {

    template <typename T>
    using KernelResult = std::expected<T, std::string>;

    class Kernel {
    public:
        explicit Kernel(JobStore &store, std::chrono::seconds lease_duration = std::chrono::seconds{30});

        Kernel(const Kernel &) = delete;
        auto operator=(const Kernel &) -> Kernel & = delete;
        Kernel(Kernel &&) = delete;
        auto operator=(Kernel &&) -> Kernel & = delete;
        ~Kernel() = default;

        [[nodiscard]]
        auto next_wakeup() const -> KernelResult<std::optional<TimePoint>>;

        [[nodiscard]]
        auto next_wakeup(TimePoint now) const -> KernelResult<std::optional<TimePoint>>;

        [[nodiscard]]
        auto reserve_due(TimePoint now, std::size_t limit, std::string_view driver_id) -> KernelResult<std::vector<DispatchRequest>>;

        [[nodiscard]]
        auto mark_started(const ExecutionId &execution_id, TimePoint now) -> KernelResult<void>;

        [[nodiscard]]
        auto mark_finished(const ExecutionId &execution_id, const ExecutionResult &result, TimePoint now) -> KernelResult<void>;

        [[nodiscard]]
        auto recover(TimePoint now, std::string_view driver_id) -> KernelResult<void>;

    private:
        struct Reservation {
            StoredJob job;
            std::int64_t lease_until = 0;
        };

        JobStore &store_;
        std::chrono::seconds lease_duration_{30};
        utils::transparent_string_unordered_map<Reservation> reservations_;
    };

} // namespace orangutan::automation
