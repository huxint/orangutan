#include "automation/planner.hpp"

#include "automation/kernel.hpp"

#include <utility>

namespace orangutan::automation {
    namespace {

        [[nodiscard]]
        auto to_schedule_spec(const TriggerDefinition &trigger) -> ScheduleSpec {
            switch (trigger.type) {
                case trigger_type::cron:
                    return CronSchedule{
                        .expr = trigger.cron,
                        .time_zone = trigger.time_zone,
                    };
                case trigger_type::interval:
                    return IntervalSchedule{
                        .every = trigger.every,
                        .jitter = trigger.jitter,
                        .active_windows = trigger.active_windows,
                        .time_zone = trigger.time_zone,
                    };
                case trigger_type::once:
                    return OneShotSchedule{
                        .at = trigger.at,
                    };
            }

            std::unreachable();
        }

        [[nodiscard]]
        auto to_job_definition(const Automation &automation) -> JobDefinition {
            return JobDefinition{
                .id = JobId{.value = automation.id},
                .key = automation.agent_key + ":" + automation.name,
                .schedule = to_schedule_spec(automation.trigger),
            };
        }

    } // namespace

    std::optional<std::int64_t> plan_next_due(const Automation &automation, TimePoint from) {
        if (!automation.enabled || automation.paused) {
            return std::nullopt;
        }
        if (automation.trigger.type == trigger_type::once && automation.last_run_at.has_value()) {
            return std::nullopt;
        }

        return plan_next_due(to_job_definition(automation), from);
    }

} // namespace orangutan::automation
