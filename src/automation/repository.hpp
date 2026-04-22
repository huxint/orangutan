#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "automation/model.hpp"
#include "storage/sqlite.hpp"

namespace orangutan::automation {

    /// Filters persisted automation definitions.
    struct AutomationQuery {
        std::string agent_key;
        std::optional<bool> enabled;
        std::optional<bool> paused;
    };

    /// Filters persisted automation run history.
    struct RunQuery {
        std::string agent_key;
        std::string automation_id;
    };

    /// Filters persisted delivery history.
    struct DeliveryQuery {
        std::string agent_key;
        std::string automation_id;
        std::string run_id;
        std::string target;
        bool only_unacked = false;
    };

    /// Represents one persisted delivery row.
    struct DeliveryRecord {
        std::string id;
        std::string run_id;
        std::string automation_id;
        std::string agent_key;
        std::string target;
        std::string status;
        std::string title;
        std::string body;
        std::int64_t created_at = 0;
        std::optional<std::int64_t> acked_at;
    };

    /// Persists unified automations, run history, and delivery history in sqlite.
    class Repository {
    public:
        Repository();
        explicit Repository(const std::filesystem::path &db_path);
        ~Repository() = default;

        Repository(const Repository &) = delete;
        Repository &operator=(const Repository &) = delete;
        Repository(Repository &&) = delete;
        Repository &operator=(Repository &&) = delete;

        [[nodiscard]]
        std::string save(const Automation &automation);

        [[nodiscard]]
        std::optional<Automation> find(std::string_view agent_key, std::string_view id_or_name) const;

        [[nodiscard]]
        std::vector<Automation> list(const AutomationQuery &query = {}) const;

        [[nodiscard]]
        bool remove(std::string_view agent_key, std::string_view id_or_name);

        [[nodiscard]]
        std::string insert_run(const RunRecord &run);

        void persist_execution(const Automation &automation, const RunRecord &run);

        void persist_delivery_results(std::string_view run_id, std::string_view delivery_status, const std::vector<DeliveryRecord> &deliveries);

        [[nodiscard]]
        std::vector<RunRecord> list_runs(const RunQuery &query = {}) const;

        [[nodiscard]]
        std::string insert_delivery(const DeliveryRecord &delivery);

        [[nodiscard]]
        std::vector<DeliveryRecord> list_deliveries(const DeliveryQuery &query = {}) const;

        [[nodiscard]]
        std::optional<DeliveryRecord> ack_delivery(std::string_view agent_key, std::string_view delivery_id, std::optional<std::int64_t> acked_at = std::nullopt);

        void clear_deliveries(const DeliveryQuery &query, std::optional<std::int64_t> acked_at = std::nullopt);

    private:
        void ensure_schema();

        sqlite::Database db_;
        mutable std::mutex mutex_;
    };

} // namespace orangutan::automation
