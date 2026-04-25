#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "automation/driver.hpp"
#include "automation/kernel.hpp"
#include "automation/sqlite-store.hpp"
#include "test-helpers.hpp"
#include "utils/task-pool.hpp"

namespace {

    using orangutan::automation::ActionDescriptor;
    using orangutan::automation::DispatchReason;
    using orangutan::automation::Driver;
    using orangutan::automation::ExecutionPolicy;
    using orangutan::automation::ExecutionResult;
    using orangutan::automation::ExecutorPort;
    using orangutan::automation::ExecutorResult;
    using orangutan::automation::IntervalSchedule;
    using orangutan::automation::JobDefinition;
    using orangutan::automation::JobId;
    using orangutan::automation::Kernel;
    using orangutan::automation::ScheduleState;
    using orangutan::automation::SqliteJobStore;

    class RecordingExecutor final : public ExecutorPort {
    public:
        auto dispatch(const orangutan::automation::DispatchRequest &request, const orangutan::automation::ExecutionContext &) -> ExecutorResult override {
            {
                std::scoped_lock lock(mutex_);
                requests_.push_back(request);
            }
            cv_.notify_all();

            return ExecutionResult{
                .success = true,
                .summary = "ok",
            };
        }

        [[nodiscard]]
        auto wait_for_requests(std::size_t count, std::chrono::milliseconds timeout) -> bool {
            std::unique_lock lock(mutex_);
            return cv_.wait_for(lock, timeout, [this, count] {
                return requests_.size() >= count;
            });
        }

        [[nodiscard]]
        auto request_count() const -> std::size_t {
            std::scoped_lock lock(mutex_);
            return requests_.size();
        }

        [[nodiscard]]
        auto request_at(std::size_t index) const -> orangutan::automation::DispatchRequest {
            std::scoped_lock lock(mutex_);
            return requests_.at(index);
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::vector<orangutan::automation::DispatchRequest> requests_;
    };

    class FlakyExecutor final : public ExecutorPort {
    public:
        explicit FlakyExecutor(int failures_before_success)
        : failures_before_success_(failures_before_success) {}

        auto dispatch(const orangutan::automation::DispatchRequest &, const orangutan::automation::ExecutionContext &) -> ExecutorResult override {
            const auto attempt = attempts_.fetch_add(1) + 1;
            if (attempt <= failures_before_success_) {
                return ExecutionResult{
                    .success = false,
                    .summary = "try again",
                };
            }
            return ExecutionResult{
                .success = true,
                .summary = "ok",
            };
        }

        [[nodiscard]]
        auto attempts() const -> int {
            return attempts_.load();
        }

    private:
        int failures_before_success_ = 0;
        std::atomic<int> attempts_{0};
    };

    [[nodiscard]]
    JobDefinition make_interval_definition(std::string_view id, std::string_view key, std::chrono::seconds every, ExecutionPolicy policy = {}) {
        return JobDefinition{
            .id = JobId{.value = std::string(id)},
            .key = std::string(key),
            .schedule =
                IntervalSchedule{
                    .every = every,
                    .jitter = std::chrono::seconds{0},
                    .active_windows = {},
                    .time_zone = "UTC",
                },
            .action =
                ActionDescriptor{
                    .action_key = "agent.prompt",
                    .payload =
                        {
                            {"agent", "default"},
                            {"prompt", "scan repo"},
                        },
                },
            .execution = policy,
        };
    }

    [[nodiscard]]
    ScheduleState make_state(std::int64_t next_due_at) {
        return ScheduleState{
            .next_due_at = next_due_at,
        };
    }

    TEST_CASE("driver stays idle when there are no due jobs", "[automation][driver]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-driver", "idle.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store);
        orangutan::utils::TaskPool pool{1};
        RecordingExecutor executor;

        std::atomic<int> clock_calls{0};
        const auto base_now = std::chrono::time_point_cast<std::chrono::seconds>(orangutan::automation::Clock::now());
        Driver driver(kernel, executor, pool, "driver-a", [&] {
            ++clock_calls;
            return base_now;
        });

        driver.start();
        std::this_thread::sleep_for(std::chrono::milliseconds{75});
        driver.stop();

        CHECK(executor.request_count() == 0);
        CHECK(clock_calls.load() <= 3);
    }

    TEST_CASE("driver wake dispatches newly due jobs", "[automation][driver]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-driver", "wake.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store);
        orangutan::utils::TaskPool pool{1};
        RecordingExecutor executor;

        const auto base_now = std::chrono::time_point_cast<std::chrono::seconds>(orangutan::automation::Clock::now());
        const auto now_seconds = orangutan::automation::to_unix_seconds(base_now);
        Driver driver(kernel, executor, pool, "driver-a", [base_now] {
            return base_now;
        });

        driver.start();
        std::this_thread::sleep_for(std::chrono::milliseconds{25});

        REQUIRE(store.save_job(make_interval_definition("job-1", "repo-sync", std::chrono::seconds{30}), make_state(now_seconds)).has_value());
        driver.wake();

        REQUIRE(executor.wait_for_requests(1, std::chrono::milliseconds{500}));
        driver.stop();

        const auto request = executor.request_at(0);
        CHECK(request.job_id.value == "job-1");
        CHECK(request.reason == DispatchReason::scheduled);
    }

    TEST_CASE("driver dispatches due jobs through the executor and updates scheduler state", "[automation][driver]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-driver", "dispatch.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store);
        orangutan::utils::TaskPool pool{1};
        RecordingExecutor executor;

        const auto base_now = std::chrono::time_point_cast<std::chrono::seconds>(orangutan::automation::Clock::now());
        const auto now_seconds = orangutan::automation::to_unix_seconds(base_now);
        REQUIRE(store.save_job(make_interval_definition("job-1", "repo-sync", std::chrono::seconds{30}), make_state(now_seconds)).has_value());

        Driver driver(kernel, executor, pool, "driver-a", [base_now] {
            return base_now;
        });

        driver.start();
        REQUIRE(executor.wait_for_requests(1, std::chrono::milliseconds{500}));
        driver.stop();

        auto loaded = store.load_job(JobId{.value = "job-1"});
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->has_value());
        REQUIRE(loaded->value().state.last_started_at.has_value());
        REQUIRE(loaded->value().state.last_finished_at.has_value());
        CHECK(*loaded->value().state.last_started_at == now_seconds);
        CHECK(*loaded->value().state.last_finished_at == now_seconds);
        CHECK(loaded->value().state.last_status == "completed");
        CHECK(loaded->value().state.in_flight_count == 0);
        CHECK(loaded->value().state.lease_owner.empty());
        CHECK_FALSE(loaded->value().state.lease_expires_at.has_value());
        REQUIRE(loaded->value().state.next_due_at.has_value());
        CHECK(*loaded->value().state.next_due_at == now_seconds + 30);
    }

    TEST_CASE("driver stop shuts down cleanly and ignores later wake signals", "[automation][driver]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-driver", "stop.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store);
        orangutan::utils::TaskPool pool{1};
        RecordingExecutor executor;

        const auto base_now = std::chrono::time_point_cast<std::chrono::seconds>(orangutan::automation::Clock::now());
        std::atomic<std::int64_t> current_seconds{orangutan::automation::to_unix_seconds(base_now)};
        Driver driver(kernel, executor, pool, "driver-a", [&] {
            return orangutan::automation::from_unix_seconds(current_seconds.load());
        });

        const auto future_due = current_seconds.load() + 3'600;
        REQUIRE(store.save_job(make_interval_definition("job-1", "repo-sync", std::chrono::seconds{30}), make_state(future_due)).has_value());

        driver.start();
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
        driver.stop();

        current_seconds.store(future_due);
        driver.wake();
        std::this_thread::sleep_for(std::chrono::milliseconds{75});

        CHECK(executor.request_count() == 0);
    }

    TEST_CASE("driver retries failed executions before recording final state", "[automation][driver]") {
        const auto db_path = orangutan::testing::unique_test_db_path("automation-driver", "retry.db");
        SqliteJobStore store(db_path);
        Kernel kernel(store);
        orangutan::utils::TaskPool pool{1};
        FlakyExecutor executor{2};

        const auto base_now = std::chrono::time_point_cast<std::chrono::seconds>(orangutan::automation::Clock::now());
        const auto now_seconds = orangutan::automation::to_unix_seconds(base_now);
        REQUIRE(store
                    .save_job(make_interval_definition("job-1", "repo-sync", std::chrono::seconds{30},
                                                       ExecutionPolicy{
                                                           .max_retry_attempts = 2,
                                                       }),
                              make_state(now_seconds))
                    .has_value());

        Driver driver(kernel, executor, pool, "driver-a", [base_now] {
            return base_now;
        });

        driver.start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (executor.attempts() < 3 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        driver.stop();

        CHECK(executor.attempts() == 3);
        auto loaded = store.load_job(JobId{.value = "job-1"});
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->has_value());
        CHECK(loaded->value().state.last_status == "completed");
    }

} // namespace
