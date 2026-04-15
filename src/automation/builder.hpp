#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "automation/model.hpp"

namespace orangutan::automation {

    /// Builds a validated unified automation value through a fluent API.
    class AutomationBuilder {
    public:
        explicit AutomationBuilder(std::string_view name);

        [[nodiscard]]
        AutomationBuilder &for_agent(std::string_view agent_key);

        [[nodiscard]]
        AutomationBuilder &run_prompt(std::string_view prompt);

        [[nodiscard]]
        AutomationBuilder &with_notes(std::string_view notes);

        [[nodiscard]]
        AutomationBuilder &cron(std::string_view expression);

        [[nodiscard]]
        AutomationBuilder &every(std::chrono::seconds cadence);

        [[nodiscard]]
        AutomationBuilder &jitter(std::chrono::seconds amount);

        [[nodiscard]]
        AutomationBuilder &once_at(TimePoint scheduled_at);

        [[nodiscard]]
        AutomationBuilder &time_zone(std::string_view zone_name);

        [[nodiscard]]
        AutomationBuilder &within_hours(ActiveWindow window);

        [[nodiscard]]
        AutomationBuilder &deliver_to(std::string_view target);

        [[nodiscard]]
        AutomationBuilder &deliver_silently();

        [[nodiscard]]
        AutomationBuilder &tag(std::string_view value);

        [[nodiscard]]
        AutomationBuilder &enable();

        [[nodiscard]]
        AutomationBuilder &disable();

        [[nodiscard]]
        Automation build() const;

    private:
        [[nodiscard]]
        TriggerDefinition build_trigger() const;

        void validate() const;

        Automation automation_;
        std::optional<trigger_type> trigger_kind_;
        std::chrono::seconds every_ = std::chrono::seconds{0};
        std::chrono::seconds jitter_ = std::chrono::seconds{0};
        TimePoint at_{};
        bool has_at_ = false;
        std::string cron_expression_;
        std::string time_zone_ = "UTC";
        std::vector<ActiveWindow> active_windows_;
    };

} // namespace orangutan::automation
