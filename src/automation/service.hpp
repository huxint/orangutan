#pragma once

#include "automation/delivery.hpp"
#include "automation/planner.hpp"
#include "automation/repository.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::automation {

    class AutomationRuntime;

    /// Owns the unified operational API for automation CRUD, execution, and delivery history.
    class AutomationService {
    public:
        using ClockSource = std::function<TimePoint()>;

        explicit AutomationService(Repository &repository, ClockSource clock = {});

        void set_executor(AutomationExecutor executor);
        void set_delivery_filter(AutomationDeliveryFilter filter);
        void set_notifier(AutomationNotifier notifier);

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
            AutomationDeliveryFilter delivery_filter;
            AutomationNotifier notifier;
        };

        [[nodiscard]]
        TimePoint current_time() const;

        [[nodiscard]]
        Callbacks callbacks() const;

        [[nodiscard]]
        Automation normalize_public_save(Automation automation, TimePoint now) const;

        [[nodiscard]]
        std::string persist(Automation automation);

        [[nodiscard]]
        std::string execute(const Automation &automation, TimePoint started_at);

        [[nodiscard]]
        std::vector<DueAutomation> collect_due(TimePoint now) const;

        void normalize_state(TimePoint now);

        Repository *repository_ = nullptr;
        ClockSource clock_;
        mutable std::mutex mutex_;
        AutomationExecutor executor_;
        AutomationDeliveryFilter delivery_filter_;
        AutomationNotifier notifier_;
    };

} // namespace orangutan::automation
