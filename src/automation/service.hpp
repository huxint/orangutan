#pragma once

#include "automation/category.hpp"
#include "automation/delivery.hpp"
#include "automation/repository.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::automation {

    class AutomationRuntime;
    class Kernel;
    class SqliteJobStore;

    /// Owns the unified operational API for automation CRUD, execution, and delivery history.
    class AutomationService {
    public:
        using ClockSource = std::function<TimePoint()>;
        using ScheduleChangedCallback = std::function<void()>;

        explicit AutomationService(Repository &repository, ClockSource clock = {});
        ~AutomationService();

        void set_executor(AutomationExecutor executor);
        void add_delivery_filter(AutomationDeliveryFilter filter);
        void register_category(AutomationCategory category);
        void set_notifier(AutomationNotifier notifier);
        void set_schedule_changed_callback(ScheduleChangedCallback callback);

        [[nodiscard]]
        std::string save(Automation automation);

        [[nodiscard]]
        std::vector<Automation> list(const AutomationQuery &query = {}) const;

        [[nodiscard]]
        std::optional<Automation> find(std::string_view agent_key, std::string_view id_or_name) const;

        [[nodiscard]]
        bool remove(std::string_view agent_key, std::string_view id_or_name);

        [[nodiscard]]
        std::string run_now(std::string_view agent_key, std::string_view id_or_name);

        [[nodiscard]]
        bool pause(std::string_view agent_key, std::string_view id_or_name);

        [[nodiscard]]
        bool resume(std::string_view agent_key, std::string_view id_or_name);

        [[nodiscard]]
        std::vector<RunRecord> list_runs(const RunQuery &query = {}) const;

        [[nodiscard]]
        std::vector<DeliveryRecord> list_deliveries(const DeliveryQuery &query = {}) const;

        [[nodiscard]]
        bool ack_delivery(std::string_view agent_key, std::string_view delivery_id);

        [[nodiscard]]
        std::string record_delivery(DeliveryRecord delivery);

        void clear_deliveries(const DeliveryQuery &query);

    private:
        friend class AutomationRuntime;

        struct Callbacks {
            AutomationExecutor executor;
            std::vector<AutomationDeliveryFilter> delivery_filters;
            AutomationNotifier notifier;
        };

        [[nodiscard]]
        TimePoint current_time() const;

        [[nodiscard]]
        Callbacks callbacks() const;

        [[nodiscard]]
        ScheduleChangedCallback schedule_changed_callback() const;

        void notify_schedule_changed() const;

        [[nodiscard]]
        Automation normalize_public_save(Automation automation, TimePoint now) const;

        [[nodiscard]]
        std::string persist(Automation automation);

        [[nodiscard]]
        Automation with_core_state(Automation automation) const;

        void sync_core_job(const Automation &automation);
        void sync_existing_core_jobs();

        struct ExecutionOutcome {
            std::string run_id;
            ExecutionResult result;
        };

        [[nodiscard]]
        auto execute_outcome(const Automation &automation, TimePoint started_at, bool sync_core_state) -> ExecutionOutcome;

        [[nodiscard]]
        std::string execute(const Automation &automation, TimePoint started_at);

        [[nodiscard]]
        Kernel &core_kernel() noexcept;

        Repository *repository_ = nullptr;
        ClockSource clock_;
        std::unique_ptr<SqliteJobStore> core_store_;
        std::unique_ptr<Kernel> core_kernel_;
        mutable std::mutex mutex_;
        AutomationExecutor executor_;
        std::vector<AutomationDeliveryFilter> delivery_filters_;
        AutomationNotifier notifier_;
        ScheduleChangedCallback schedule_changed_callback_;
    };

} // namespace orangutan::automation
