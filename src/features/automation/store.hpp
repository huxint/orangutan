#pragma once

#include "features/automation/types.hpp"
#include "infra/storage/sqlite.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace orangutan::automation {

    class Store {
    public:
        Store();
        explicit Store(const std::filesystem::path &db_path);
        ~Store() = default;

        Store(const Store &) = delete;
        Store &operator=(const Store &) = delete;
        Store(Store &&) = delete;
        Store &operator=(Store &&) = delete;

        [[nodiscard]]
        std::vector<TaskSpec> list_tasks(const std::string &agent_key = {}) const;
        [[nodiscard]]
        std::optional<TaskSpec> find_task(const std::string &agent_key, const std::string &id_or_name) const;
        [[nodiscard]]
        std::string upsert_task(const TaskSpec &task);
        [[nodiscard]]
        bool remove_task(const std::string &agent_key, const std::string &id_or_name);
        void update_task_run_state(const std::string &task_id, std::optional<orangutan::base::i64> last_run_at, std::string_view last_status, bool enabled);

        [[nodiscard]]
        std::vector<HeartbeatSpec> list_heartbeats(const std::string &agent_key = {}) const;
        [[nodiscard]]
        std::optional<HeartbeatSpec> find_heartbeat(const std::string &agent_key, const std::string &id_or_name) const;
        [[nodiscard]]
        std::string upsert_heartbeat(const HeartbeatSpec &heartbeat);
        [[nodiscard]]
        bool remove_heartbeat(const std::string &agent_key, const std::string &id_or_name);
        void update_heartbeat_run_state(const std::string &heartbeat_id, std::optional<orangutan::base::i64> last_run_at, std::optional<orangutan::base::i64> next_due_at,
                                        std::string_view last_status, bool paused);

        [[nodiscard]]
        std::string insert_run(const RunRecord &run);
        void complete_run(const std::string &run_id, std::string_view status, std::string_view summary, std::string_view delivery_status, std::string_view log_path,
                          std::optional<orangutan::base::i64> finished_at);
        [[nodiscard]]
        std::vector<RunRecord> list_runs(const std::string &agent_key = {}) const;

        [[nodiscard]]
        std::string insert_inbox(const InboxItem &item);
        [[nodiscard]]
        std::vector<InboxItem> list_inbox(const std::string &agent_key) const;
        [[nodiscard]]
        bool ack_inbox(const std::string &agent_key, const std::string &id);
        void clear_inbox(const std::string &agent_key);

    private:
        sqlite::Database db_;
        mutable std::mutex mutex_;

        void ensure_schema();
    };

} // namespace orangutan::automation
