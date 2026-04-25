#pragma once

#include "automation/core-model.hpp"
#include "storage/sqlite-error.hpp"

#include <expected>
#include <optional>
#include <string_view>
#include <vector>

namespace orangutan::automation {

    using StoreError = sqlite::SqliteError;

    template <typename T>
    using StoreResult = std::expected<T, StoreError>;

    struct StoredJob {
        JobDefinition definition;
        ScheduleState state;
    };

    class JobStore {
    public:
        virtual ~JobStore() = default;

        JobStore(const JobStore &) = delete;
        auto operator=(const JobStore &) -> JobStore & = delete;
        JobStore(JobStore &&) = delete;
        auto operator=(JobStore &&) -> JobStore & = delete;

        [[nodiscard]]
        virtual auto save_job(const JobDefinition &definition, const ScheduleState &state) -> StoreResult<void> = 0;

        [[nodiscard]]
        virtual auto load_job(const JobId &job_id) const -> StoreResult<std::optional<StoredJob>> = 0;

        [[nodiscard]]
        virtual auto remove_job(const JobId &job_id) -> StoreResult<bool> = 0;

        [[nodiscard]]
        virtual auto next_due_at() const -> StoreResult<std::optional<std::int64_t>> = 0;

        [[nodiscard]]
        virtual auto next_wakeup(std::int64_t now) const -> StoreResult<std::optional<std::int64_t>> = 0;

        [[nodiscard]]
        virtual auto list_due(std::int64_t now, std::size_t limit) const -> StoreResult<std::vector<JobId>> = 0;

        [[nodiscard]]
        virtual auto reserve_due(std::int64_t now, std::size_t limit, std::string_view driver_id, std::int64_t lease_until) -> StoreResult<std::vector<StoredJob>> = 0;

        [[nodiscard]]
        virtual auto recover_expired_leases(std::int64_t now, std::string_view driver_id) -> StoreResult<int> = 0;

    protected:
        JobStore() = default;
    };

} // namespace orangutan::automation
