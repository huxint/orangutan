#include "automation/kernel.hpp"

#include "automation/cron-parser.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <utility>

namespace orangutan::automation {

    namespace {

        constexpr int MAX_CRON_SCAN_MINUTES = 2 * 366 * 24 * 60;

        [[nodiscard]]
        auto generate_execution_id() -> ExecutionId {
            static std::atomic<std::uint64_t> counter{0};
            const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            return ExecutionId{
                .value = "exec-" + std::to_string(now) + "-" + std::to_string(sequence),
            };
        }

        [[nodiscard]]
        auto resolve_time_zone(std::string_view zone_name) -> const std::chrono::time_zone * {
            if (zone_name.empty() || zone_name == "UTC") {
                return std::chrono::locate_zone("UTC");
            }
            return std::chrono::locate_zone(std::string(zone_name));
        }

        [[nodiscard]]
        auto local_time_in_zone(TimePoint time_point, std::string_view zone_name) -> std::chrono::local_seconds {
            const auto zone = resolve_time_zone(zone_name);
            return std::chrono::zoned_time{zone, std::chrono::floor<std::chrono::seconds>(time_point)}.get_local_time();
        }

        [[nodiscard]]
        auto next_cron_fire_time(const CronSchedule &schedule, TimePoint after) -> std::optional<TimePoint> {
            const auto parsed = parse_cron_silent(schedule.expr);
            if (!parsed.has_value()) {
                return std::nullopt;
            }

            auto candidate = std::chrono::ceil<std::chrono::minutes>(after + std::chrono::seconds{1});
            for (int index = 0; index < MAX_CRON_SCAN_MINUTES; ++index) {
                if (cron_matches_local(*parsed, local_time_in_zone(candidate, schedule.time_zone))) {
                    return candidate;
                }
                candidate += std::chrono::minutes{1};
            }

            return std::nullopt;
        }

        [[nodiscard]]
        auto positive_jitter_offset(const JobDefinition &definition, TimePoint base_time, std::chrono::seconds jitter) -> std::int64_t {
            if (jitter <= std::chrono::seconds{0}) {
                return 0;
            }

            const auto seed_material = definition.id.value + ":" + std::to_string(to_unix_seconds(base_time));
            std::mt19937_64 generator(static_cast<std::uint64_t>(std::hash<std::string>{}(seed_material)));
            std::uniform_int_distribution<std::int64_t> distribution(0, jitter.count());
            return distribution(generator);
        }

        [[nodiscard]]
        auto within_active_windows(const IntervalSchedule &schedule, TimePoint candidate) -> bool {
            if (schedule.active_windows.empty()) {
                return true;
            }

            const auto local_time = local_time_in_zone(candidate, schedule.time_zone);
            const auto local_day = std::chrono::floor<std::chrono::days>(local_time);
            const auto tod = std::chrono::hh_mm_ss{local_time - local_day};
            const auto minute_of_day = std::chrono::minutes{tod.hours()} + std::chrono::minutes{tod.minutes()};

            return std::ranges::any_of(schedule.active_windows, [minute_of_day](const ActiveWindow &window) {
                return minute_of_day >= window.start && minute_of_day < window.end;
            });
        }

        [[nodiscard]]
        auto clamp_to_active_windows(const IntervalSchedule &schedule, TimePoint candidate) -> TimePoint {
            if (schedule.active_windows.empty() || within_active_windows(schedule, candidate)) {
                return candidate;
            }

            auto windows = schedule.active_windows;
            std::ranges::sort(windows, {}, &ActiveWindow::start);

            const auto zone = resolve_time_zone(schedule.time_zone);
            const auto local_candidate = local_time_in_zone(candidate, schedule.time_zone);
            const auto local_day = std::chrono::floor<std::chrono::days>(local_candidate);
            const auto tod = std::chrono::hh_mm_ss{local_candidate - local_day};
            const auto minute_of_day = std::chrono::minutes{tod.hours()} + std::chrono::minutes{tod.minutes()};

            for (int day_offset = 0; day_offset < 8; ++day_offset) {
                const auto candidate_day = local_day + std::chrono::days{day_offset};
                for (const auto &window : windows) {
                    if (day_offset == 0 && minute_of_day < window.start) {
                        const auto next_local = std::chrono::time_point_cast<std::chrono::seconds>(candidate_day + window.start);
                        return std::chrono::time_point_cast<Clock::duration>(zone->to_sys(next_local, std::chrono::choose::latest));
                    }
                    if (day_offset > 0) {
                        const auto next_local = std::chrono::time_point_cast<std::chrono::seconds>(candidate_day + window.start);
                        return std::chrono::time_point_cast<Clock::duration>(zone->to_sys(next_local, std::chrono::choose::latest));
                    }
                }
            }

            return candidate;
        }

        [[nodiscard]]
        auto plan_job_next_due(const JobDefinition &definition, TimePoint from) -> std::optional<std::int64_t> {
            const auto planned = std::visit(
                [&](const auto &schedule) -> std::optional<TimePoint> {
                    using T = std::decay_t<decltype(schedule)>;
                    if constexpr (std::is_same_v<T, CronSchedule>) {
                        return next_cron_fire_time(schedule, from);
                    } else if constexpr (std::is_same_v<T, IntervalSchedule>) {
                        if (schedule.every <= std::chrono::seconds{0}) {
                            return std::nullopt;
                        }
                        auto candidate = from + schedule.every + std::chrono::seconds{positive_jitter_offset(definition, from, schedule.jitter)};
                        candidate = clamp_to_active_windows(schedule, candidate);
                        return candidate;
                    } else {
                        return schedule.at;
                    }
                },
                definition.schedule);

            if (!planned.has_value()) {
                return std::nullopt;
            }
            return to_unix_seconds(*planned);
        }

        [[nodiscard]]
        auto make_dispatch_request(const StoredJob &stored_job, std::int64_t now_seconds) -> std::optional<DispatchRequest> {
            if (!stored_job.state.next_due_at.has_value()) {
                return std::nullopt;
            }

            const auto scheduled_for = *stored_job.state.next_due_at;
            if (scheduled_for > now_seconds) {
                return std::nullopt;
            }

            if (stored_job.definition.execution.missed_runs == MissedRunPolicy::skip && scheduled_for < now_seconds) {
                return std::nullopt;
            }

            return DispatchRequest{
                .job_id = stored_job.definition.id,
                .execution_id = generate_execution_id(),
                .scheduled_for = from_unix_seconds(scheduled_for),
                .reason = scheduled_for < now_seconds ? DispatchReason::catch_up : DispatchReason::scheduled,
                .action = stored_job.definition.action,
                .execution = stored_job.definition.execution,
                .result = stored_job.definition.result,
            };
        }

    } // namespace

    auto plan_next_due(const JobDefinition &definition, TimePoint from) -> std::optional<std::int64_t> {
        return plan_job_next_due(definition, from);
    }

    Kernel::Kernel(JobStore &store, std::chrono::seconds lease_duration)
    : store_(store),
      lease_duration_(lease_duration) {}

    auto Kernel::next_wakeup() const -> KernelResult<std::optional<TimePoint>> {
        auto next_due_at = store_.next_due_at();
        if (!next_due_at) {
            return std::unexpected(next_due_at.error().message);
        }
        if (!next_due_at->has_value()) {
            return std::optional<TimePoint>{};
        }
        return std::optional<TimePoint>{from_unix_seconds(**next_due_at)};
    }

    auto Kernel::next_wakeup(TimePoint now) const -> KernelResult<std::optional<TimePoint>> {
        auto next_wakeup = store_.next_wakeup(to_unix_seconds(now));
        if (!next_wakeup) {
            return std::unexpected(next_wakeup.error().message);
        }
        if (!next_wakeup->has_value()) {
            return std::optional<TimePoint>{};
        }
        return std::optional<TimePoint>{from_unix_seconds(**next_wakeup)};
    }

    auto Kernel::reserve_due(TimePoint now, std::size_t limit, std::string_view driver_id) -> KernelResult<std::vector<DispatchRequest>> {
        const auto now_seconds = to_unix_seconds(now);
        const auto lease_until = now_seconds + lease_duration_.count();

        auto reserved_jobs = store_.reserve_due(now_seconds, limit, driver_id, lease_until);
        if (!reserved_jobs) {
            return std::unexpected(reserved_jobs.error().message);
        }

        std::vector<DispatchRequest> dispatches;
        dispatches.reserve(reserved_jobs->size());
        for (auto &reserved_job : *reserved_jobs) {
            auto request = make_dispatch_request(reserved_job, now_seconds);
            if (!request.has_value()) {
                reserved_job.state.next_due_at = plan_job_next_due(reserved_job.definition, now);
                reserved_job.state.lease_owner.clear();
                reserved_job.state.lease_expires_at.reset();
                auto saved = store_.save_job(reserved_job.definition, reserved_job.state);
                if (!saved) {
                    return std::unexpected(saved.error().message);
                }
                continue;
            }

            reserved_job.state.last_scheduled_at = to_unix_seconds(request->scheduled_for);
            auto saved = store_.save_job(reserved_job.definition, reserved_job.state);
            if (!saved) {
                return std::unexpected(saved.error().message);
            }

            reservations_.insert_or_assign(request->execution_id.value, Reservation{
                                                                            .job = std::move(reserved_job),
                                                                            .scheduled_for = to_unix_seconds(request->scheduled_for),
                                                                            .lease_until = lease_until,
                                                                        });
            dispatches.push_back(*request);
        }

        return dispatches;
    }

    auto Kernel::mark_started(const ExecutionId &execution_id, TimePoint now) -> KernelResult<void> {
        const auto it = utils::transparent_find(reservations_, execution_id.value);
        if (it == reservations_.end()) {
            return std::unexpected("unknown execution id");
        }

        it->second.job.state.last_started_at = to_unix_seconds(now);
        ++it->second.job.state.in_flight_count;
        auto saved = store_.save_job(it->second.job.definition, it->second.job.state);
        if (!saved) {
            return std::unexpected(saved.error().message);
        }
        return {};
    }

    auto Kernel::mark_finished(const ExecutionId &execution_id, const ExecutionResult &result, TimePoint now) -> KernelResult<void> {
        const auto it = utils::transparent_find(reservations_, execution_id.value);
        if (it == reservations_.end()) {
            return std::unexpected("unknown execution id");
        }

        auto &job = it->second.job;
        job.state.last_finished_at = to_unix_seconds(now);
        if (job.state.in_flight_count > 0) {
            --job.state.in_flight_count;
        }
        job.state.last_status = result.success ? "completed" : "failed";
        job.state.lease_owner.clear();
        job.state.lease_expires_at.reset();
        if (std::holds_alternative<OneShotSchedule>(job.definition.schedule)) {
            job.state.next_due_at.reset();
        } else {
            const auto next_due_base = job.definition.execution.missed_runs == MissedRunPolicy::catch_up ? from_unix_seconds(it->second.scheduled_for) : now;
            job.state.next_due_at = plan_job_next_due(job.definition, next_due_base);
        }

        auto saved = store_.save_job(job.definition, job.state);
        if (!saved) {
            return std::unexpected(saved.error().message);
        }

        reservations_.erase(it);
        return {};
    }

    auto Kernel::recover(TimePoint now, std::string_view driver_id) -> KernelResult<void> {
        const auto now_seconds = to_unix_seconds(now);
        for (auto it = reservations_.begin(); it != reservations_.end();) {
            if (it->second.lease_until <= now_seconds) {
                it = reservations_.erase(it);
                continue;
            }
            ++it;
        }

        auto recovered = store_.recover_expired_leases(now_seconds, driver_id);
        if (!recovered) {
            return std::unexpected(recovered.error().message);
        }
        return {};
    }

} // namespace orangutan::automation
