#include "automation/dsl.hpp"

#include "automation/cron-parser.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace orangutan::automation {

    namespace {

        constexpr auto FULL_DAY_MINUTES = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::hours{24});

        [[nodiscard]]
        auto is_blank(std::string_view value) -> bool {
            return std::ranges::all_of(value, [](unsigned char ch) {
                return std::isspace(ch) != 0;
            });
        }

        [[nodiscard]]
        auto validate_time_zone_name(std::string_view value) -> std::optional<std::string> {
            if (is_blank(value)) {
                return "time zone must not be blank";
            }
            if (value == "UTC") {
                return std::nullopt;
            }

            try {
                static_cast<void>(std::chrono::locate_zone(std::string(value)));
            } catch (const std::runtime_error &) {
                return "time zone must be UTC or a valid IANA zone name";
            }

            return std::nullopt;
        }

    } // namespace

    CronBuilder::CronBuilder(std::string_view expression)
    : schedule_{
          .expr = std::string(expression),
      } {}

    auto CronBuilder::build() const -> ScheduleSpec {
        return schedule_;
    }

    IntervalBuilder::IntervalBuilder(std::chrono::seconds cadence)
    : schedule_{
          .every = cadence,
      } {}

    auto IntervalBuilder::build() const -> ScheduleSpec {
        return schedule_;
    }

    OneShotBuilder::OneShotBuilder(TimePoint scheduled_at)
    : schedule_{
          .at = scheduled_at,
      } {}

    auto OneShotBuilder::build() const -> ScheduleSpec {
        return schedule_;
    }

    auto PipelineBuilder::build() const -> DslResult<ActionDescriptor> {
        if (error_.has_value()) {
            return std::unexpected(*error_);
        }
        if (steps_.empty()) {
            return std::unexpected("pipeline must contain at least one step");
        }

        nlohmann::json steps = nlohmann::json::array();
        for (const auto &step : steps_) {
            steps.push_back({
                {"action_key", step.action_key},
                {"payload", step.payload},
            });
        }

        return ActionDescriptor{
            .action_key = "pipeline",
            .payload =
                nlohmann::json{
                    {"steps", std::move(steps)},
                },
        };
    }

    JobBuilder::JobBuilder(std::string_view key) {
        definition_.key = std::string(key);
    }

    auto JobBuilder::build() const -> DslResult<JobDefinition> {
        if (auto error = validate(); error.has_value()) {
            return std::unexpected(std::move(*error));
        }

        auto definition = definition_;
        definition.id.value = definition.id.value.empty() ? generate_id("job") : definition.id.value;
        definition.schedule = *schedule_;
        return definition;
    }

    auto JobBuilder::validate() const -> std::optional<std::string> {
        if (error_.has_value()) {
            return error_;
        }
        if (is_blank(definition_.key)) {
            return "job key must not be blank";
        }
        if (!schedule_.has_value()) {
            return "schedule must be configured";
        }
        if (const auto error = validate_schedule(*schedule_)) {
            return error;
        }
        if (is_blank(definition_.action.action_key)) {
            return "action binding must be configured";
        }
        if (definition_.execution.max_retry_attempts < 0) {
            return "retry attempts must not be negative";
        }
        if (definition_.execution.initial_backoff < std::chrono::milliseconds{0}) {
            return "initial retry backoff must not be negative";
        }
        if (definition_.execution.max_backoff < std::chrono::milliseconds{0}) {
            return "max retry backoff must not be negative";
        }
        if (definition_.execution.max_backoff < definition_.execution.initial_backoff) {
            return "max retry backoff must be at least the initial retry backoff";
        }
        if (definition_.result.mode == delivery_mode::notify && definition_.result.targets.empty()) {
            return "notify delivery requires at least one target";
        }
        if (!std::ranges::all_of(definition_.result.targets, [](std::string_view target) {
                return !is_blank(target);
            })) {
            return "delivery target must not be blank";
        }
        return std::nullopt;
    }

    auto JobBuilder::validate_schedule(const ScheduleSpec &schedule) -> std::optional<std::string> {
        return std::visit(
            [](const auto &spec) -> std::optional<std::string> {
                using T = std::decay_t<decltype(spec)>;
                if constexpr (std::is_same_v<T, CronSchedule>) {
                    if (is_blank(spec.expr)) {
                        return "cron expression must not be blank";
                    }
                    if (!parse_cron_silent(spec.expr).has_value()) {
                        return "cron expression is invalid";
                    }
                    return validate_time_zone_name(spec.time_zone);
                } else if constexpr (std::is_same_v<T, IntervalSchedule>) {
                    if (spec.every <= std::chrono::seconds{0}) {
                        return "interval cadence must be positive";
                    }
                    if (spec.jitter < std::chrono::seconds{0}) {
                        return "interval jitter must not be negative";
                    }
                    if (const auto error = validate_time_zone_name(spec.time_zone)) {
                        return error;
                    }
                    for (const auto &window : spec.active_windows) {
                        if (window.start < std::chrono::minutes{0} || window.end > FULL_DAY_MINUTES || window.start >= window.end) {
                            return "active windows must be within one day and start before end";
                        }
                    }
                    return std::nullopt;
                } else {
                    return std::nullopt;
                }
            },
            schedule);
    }

    auto cron(std::string_view expression) -> CronBuilder {
        return CronBuilder(expression);
    }

    auto every(std::chrono::seconds cadence) -> IntervalBuilder {
        return IntervalBuilder(cadence);
    }

    auto once_at(TimePoint scheduled_at) -> OneShotBuilder {
        return OneShotBuilder(scheduled_at);
    }

    auto pipeline() -> PipelineBuilder {
        return PipelineBuilder{};
    }

    auto job(std::string_view key) -> JobBuilder {
        return JobBuilder(key);
    }

} // namespace orangutan::automation
