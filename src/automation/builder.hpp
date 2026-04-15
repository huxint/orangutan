#pragma once

#include <chrono>
#include <expected>
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

        auto for_agent(this auto &&self, std::string_view agent_key) -> decltype(auto) {
            self.automation_.agent_key = std::string(agent_key);
            return std::forward<decltype(self)>(self);
        }

        auto run_prompt(this auto &&self, std::string_view prompt) -> decltype(auto) {
            self.automation_.prompt = std::string(prompt);
            return std::forward<decltype(self)>(self);
        }

        auto with_notes(this auto &&self, std::string_view notes) -> decltype(auto) {
            self.automation_.notes = std::string(notes);
            return std::forward<decltype(self)>(self);
        }

        auto cron(this auto &&self, std::string_view expression) -> decltype(auto) {
            self.trigger_kind_ = trigger_type::cron;
            self.cron_expression_ = std::string(expression);
            self.every_ = std::chrono::seconds{0};
            self.jitter_ = std::chrono::seconds{0};
            self.active_windows_.clear();
            self.has_at_ = false;
            return std::forward<decltype(self)>(self);
        }

        auto every(this auto &&self, std::chrono::seconds cadence) -> decltype(auto) {
            self.trigger_kind_ = trigger_type::interval;
            self.cron_expression_.clear();
            self.every_ = cadence;
            self.has_at_ = false;
            return std::forward<decltype(self)>(self);
        }

        auto jitter(this auto &&self, std::chrono::seconds amount) -> decltype(auto) {
            self.jitter_ = amount;
            return std::forward<decltype(self)>(self);
        }

        auto once_at(this auto &&self, TimePoint scheduled_at) -> decltype(auto) {
            self.trigger_kind_ = trigger_type::once;
            self.cron_expression_.clear();
            self.every_ = std::chrono::seconds{0};
            self.jitter_ = std::chrono::seconds{0};
            self.active_windows_.clear();
            self.time_zone_ = "UTC";
            self.at_ = scheduled_at;
            self.has_at_ = true;
            return std::forward<decltype(self)>(self);
        }

        auto time_zone(this auto &&self, std::string_view zone_name) -> decltype(auto) {
            self.time_zone_ = std::string(zone_name);
            return std::forward<decltype(self)>(self);
        }

        auto within_hours(this auto &&self, ActiveWindow window) -> decltype(auto) {
            self.active_windows_.emplace_back(window);
            return std::forward<decltype(self)>(self);
        }

        auto deliver_to(this auto &&self, std::string_view target) -> decltype(auto) {
            self.automation_.delivery.mode = delivery_mode::notify;
            self.automation_.delivery.targets.emplace_back(target);
            return std::forward<decltype(self)>(self);
        }

        auto deliver_silently(this auto &&self) -> decltype(auto) {
            self.automation_.delivery.mode = delivery_mode::silent;
            self.automation_.delivery.targets.clear();
            return std::forward<decltype(self)>(self);
        }

        auto tag(this auto &&self, std::string_view value) -> decltype(auto) {
            self.automation_.tags.emplace_back(value);
            return std::forward<decltype(self)>(self);
        }

        auto enable(this auto &&self) -> decltype(auto) {
            self.automation_.enabled = true;
            self.automation_.paused = false;
            return std::forward<decltype(self)>(self);
        }

        auto disable(this auto &&self) -> decltype(auto) {
            self.automation_.enabled = false;
            self.automation_.paused = false;
            self.automation_.next_due_at.reset();
            return std::forward<decltype(self)>(self);
        }

        [[nodiscard]]
        auto build() const -> std::expected<Automation, std::string>;

    private:
        [[nodiscard]]
        TriggerDefinition build_trigger() const;

        [[nodiscard]]
        std::optional<std::string> validate() const;

        Automation automation_;
        std::optional<trigger_type> trigger_kind_;
        std::chrono::seconds every_ = std::chrono::seconds{0};
        std::chrono::seconds jitter_ = std::chrono::seconds{0};
        TimePoint at_;
        bool has_at_ = false;
        std::string cron_expression_;
        std::string time_zone_ = "UTC";
        std::vector<ActiveWindow> active_windows_;
    };

} // namespace orangutan::automation
