#pragma once

#include "automation/store.hpp"
#include "storage/sqlite.hpp"

#include <filesystem>
#include <mutex>

namespace orangutan::automation {

    class SqliteJobStore final : public JobStore {
    public:
        explicit SqliteJobStore(const std::filesystem::path &db_path);
        ~SqliteJobStore() override = default;

        SqliteJobStore(const SqliteJobStore &) = delete;
        auto operator=(const SqliteJobStore &) -> SqliteJobStore & = delete;
        SqliteJobStore(SqliteJobStore &&) = delete;
        auto operator=(SqliteJobStore &&) -> SqliteJobStore & = delete;

        [[nodiscard]]
        auto save_job(const JobDefinition &definition, const ScheduleState &state) -> StoreResult<void> override;

        [[nodiscard]]
        auto load_job(const JobId &job_id) const -> StoreResult<std::optional<StoredJob>> override;

        [[nodiscard]]
        auto remove_job(const JobId &job_id) -> StoreResult<bool> override;

        [[nodiscard]]
        auto next_due_at() const -> StoreResult<std::optional<std::int64_t>> override;

        [[nodiscard]]
        auto list_due(std::int64_t now, std::size_t limit) const -> StoreResult<std::vector<JobId>> override;

    private:
        auto ensure_schema() -> StoreResult<void>;

        mutable std::mutex mutex_;
        sqlite::Database db_;
    };

} // namespace orangutan::automation
