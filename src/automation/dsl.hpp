#pragma once

#include "automation/action-registry.hpp"
#include "automation/core-model.hpp"

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::automation {

    using DslError = std::string;

    template <typename T>
    using DslResult = std::expected<T, DslError>;

    template <typename Payload>
    auto make_action_descriptor(std::string_view action_key, const Payload &payload) -> DslResult<ActionDescriptor> {
        if (action_key.empty()) {
            return std::unexpected("action key must not be blank");
        }

        auto encoded = encode_action_payload(payload);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }

        return ActionDescriptor{
            .action_key = std::string(action_key),
            .payload = std::move(*encoded),
        };
    }

    class CronBuilder {
    public:
        explicit CronBuilder(std::string_view expression);

        auto time_zone(this auto &&self, std::string_view zone_name) -> decltype(auto) {
            self.schedule_.time_zone = std::string(zone_name);
            return std::forward<decltype(self)>(self);
        }

        [[nodiscard]]
        auto build() const -> ScheduleSpec;

    private:
        CronSchedule schedule_;
    };

    class IntervalBuilder {
    public:
        explicit IntervalBuilder(std::chrono::seconds cadence);

        auto jitter(this auto &&self, std::chrono::seconds amount) -> decltype(auto) {
            self.schedule_.jitter = amount;
            return std::forward<decltype(self)>(self);
        }

        auto time_zone(this auto &&self, std::string_view zone_name) -> decltype(auto) {
            self.schedule_.time_zone = std::string(zone_name);
            return std::forward<decltype(self)>(self);
        }

        auto within_hours(this auto &&self, ActiveWindow window) -> decltype(auto) {
            self.schedule_.active_windows.push_back(window);
            return std::forward<decltype(self)>(self);
        }

        [[nodiscard]]
        auto build() const -> ScheduleSpec;

    private:
        IntervalSchedule schedule_;
    };

    class OneShotBuilder {
    public:
        explicit OneShotBuilder(TimePoint scheduled_at);

        [[nodiscard]]
        auto build() const -> ScheduleSpec;

    private:
        OneShotSchedule schedule_;
    };

    class PipelineBuilder {
    public:
        PipelineBuilder() = default;

        template <typename Payload>
        auto step(this auto &&self, std::string_view action_key, const Payload &payload) -> decltype(auto) {
            auto step = make_action_descriptor(action_key, payload);
            if (!step) {
                self.error_ = step.error();
                return std::forward<decltype(self)>(self);
            }
            self.steps_.push_back(std::move(*step));
            return std::forward<decltype(self)>(self);
        }

        auto step(this auto &&self, ActionDescriptor action) -> decltype(auto) {
            if (action.action_key.empty()) {
                self.error_ = "pipeline step action key must not be blank";
                return std::forward<decltype(self)>(self);
            }
            self.steps_.push_back(std::move(action));
            return std::forward<decltype(self)>(self);
        }

        template <typename Payload>
        auto then(this auto &&self, std::string_view action_key, const Payload &payload) -> decltype(auto) {
            return std::forward<decltype(self)>(self).step(action_key, payload);
        }

        auto then(this auto &&self, ActionDescriptor action) -> decltype(auto) {
            return std::forward<decltype(self)>(self).step(std::move(action));
        }

        [[nodiscard]]
        auto build() const -> DslResult<ActionDescriptor>;

    private:
        std::optional<std::string> error_;
        std::vector<ActionDescriptor> steps_;
    };

    class JobBuilder {
    public:
        explicit JobBuilder(std::string_view key);

        auto id(this auto &&self, std::string_view value) -> decltype(auto) {
            self.definition_.id.value = std::string(value);
            return std::forward<decltype(self)>(self);
        }

        auto version(this auto &&self, std::int64_t value) -> decltype(auto) {
            self.definition_.version = value;
            return std::forward<decltype(self)>(self);
        }

        auto metadata(this auto &&self, nlohmann::json value) -> decltype(auto) {
            self.definition_.metadata = std::move(value);
            return std::forward<decltype(self)>(self);
        }

        auto schedule(this auto &&self, ScheduleSpec value) -> decltype(auto) {
            self.schedule_ = std::move(value);
            return std::forward<decltype(self)>(self);
        }

        auto schedule(this auto &&self, const CronBuilder &builder) -> decltype(auto) {
            return std::forward<decltype(self)>(self).schedule(builder.build());
        }

        auto schedule(this auto &&self, const IntervalBuilder &builder) -> decltype(auto) {
            return std::forward<decltype(self)>(self).schedule(builder.build());
        }

        auto schedule(this auto &&self, const OneShotBuilder &builder) -> decltype(auto) {
            return std::forward<decltype(self)>(self).schedule(builder.build());
        }

        auto missed_run(this auto &&self, MissedRunPolicy policy) -> decltype(auto) {
            self.definition_.execution.missed_runs = policy;
            return std::forward<decltype(self)>(self);
        }

        auto overlap(this auto &&self, OverlapPolicy policy) -> decltype(auto) {
            self.definition_.execution.overlap = policy;
            self.definition_.execution.allow_parallel = (policy == OverlapPolicy::parallel);
            return std::forward<decltype(self)>(self);
        }

        auto retry(this auto &&self, int attempts, std::chrono::milliseconds initial_backoff, std::chrono::milliseconds max_backoff) -> decltype(auto) {
            self.definition_.execution.max_retry_attempts = attempts;
            self.definition_.execution.initial_backoff = initial_backoff;
            self.definition_.execution.max_backoff = max_backoff;
            return std::forward<decltype(self)>(self);
        }

        auto deliver_to(this auto &&self, std::string_view target) -> decltype(auto) {
            self.definition_.result.mode = delivery_mode::notify;
            self.definition_.result.targets.emplace_back(target);
            return std::forward<decltype(self)>(self);
        }

        auto deliver_silently(this auto &&self) -> decltype(auto) {
            self.definition_.result.mode = delivery_mode::silent;
            self.definition_.result.targets.clear();
            return std::forward<decltype(self)>(self);
        }

        auto persist_full_reply(this auto &&self, bool enabled = true) -> decltype(auto) {
            self.definition_.result.persist_full_reply = enabled;
            return std::forward<decltype(self)>(self);
        }

        template <typename Payload>
        auto bind(this auto &&self, std::string_view action_key, const Payload &payload) -> decltype(auto) {
            auto action = make_action_descriptor(action_key, payload);
            if (!action) {
                self.error_ = action.error();
                return std::forward<decltype(self)>(self);
            }
            self.definition_.action = std::move(*action);
            return std::forward<decltype(self)>(self);
        }

        auto bind(this auto &&self, ActionDescriptor action) -> decltype(auto) {
            self.definition_.action = std::move(action);
            return std::forward<decltype(self)>(self);
        }

        [[nodiscard]]
        auto build() const -> DslResult<JobDefinition>;

    private:
        [[nodiscard]]
        auto validate() const -> std::optional<std::string>;

        [[nodiscard]]
        static auto validate_schedule(const ScheduleSpec &schedule) -> std::optional<std::string>;

        JobDefinition definition_;
        std::optional<ScheduleSpec> schedule_;
        std::optional<std::string> error_;
    };

    [[nodiscard]]
    auto cron(std::string_view expression) -> CronBuilder;

    [[nodiscard]]
    auto every(std::chrono::seconds cadence) -> IntervalBuilder;

    [[nodiscard]]
    auto once_at(TimePoint scheduled_at) -> OneShotBuilder;

    [[nodiscard]]
    auto pipeline() -> PipelineBuilder;

    [[nodiscard]]
    auto job(std::string_view key) -> JobBuilder;

} // namespace orangutan::automation
