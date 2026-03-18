#include "features/heartbeat/scheduler.hpp"
#include "features/heartbeat/protocol/heartbeat-ok.hpp"
#include "features/channel/core/channel.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

using namespace orangutan;

namespace {

void wait_until_not_near_minute_boundary() {
    auto now = std::chrono::system_clock::now();
    const auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto second_in_minute = seconds_since_epoch % 60;

    if (second_in_minute >= 58) {
        const auto sleep_for = std::chrono::seconds(61 - second_in_minute) + std::chrono::milliseconds(100);
        std::this_thread::sleep_for(sleep_for);
    }
}


std::filesystem::path make_test_path(std::string_view filename) {
    return std::filesystem::temp_directory_path() / filename;
}

bool write_test_file(const std::filesystem::path &path, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }

    file << content;
    return file.good();
}
} // namespace

TEST(HeartbeatSchedulerTest, AddJobRegistersJob) {
    HeartbeatScheduler scheduler([](const HeartbeatJob &) {});

    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("test-job", *expr, "default", "cli", "Hello");

    EXPECT_EQ(scheduler.jobs().size(), 1);
    EXPECT_EQ(scheduler.jobs()[0].name, "test-job");
    EXPECT_EQ(scheduler.jobs()[0].prompt, "Hello");
}

TEST(HeartbeatSchedulerTest, StartAndStopGracefully) {
    HeartbeatScheduler scheduler([](const HeartbeatJob &) {});

    auto expr = parse_cron("0 0 1 1 *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("far-future", *expr, "default", "cli", "test");

    scheduler.start();
    scheduler.stop();
}

TEST(HeartbeatSchedulerTest, RepeatedStartDoesNotTerminateProcess) {
    EXPECT_EXIT(
        {
            HeartbeatScheduler scheduler([](const HeartbeatJob &) {});

            auto expr = parse_cron("0 0 1 1 *");
            if (!expr.has_value()) {
                std::_Exit(2);
            }

            scheduler.add_job("far-future", *expr, "default", "cli", "test");
            scheduler.start();
            scheduler.start();
            scheduler.stop();
            std::_Exit(0);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST(HeartbeatSchedulerTest, StartAfterStopDoesNotTerminateProcess) {
    EXPECT_EXIT(
        {
            HeartbeatScheduler scheduler([](const HeartbeatJob &) {});

            auto expr = parse_cron("0 0 1 1 *");
            if (!expr.has_value()) {
                std::_Exit(2);
            }

            scheduler.add_job("far-future", *expr, "default", "cli", "test");
            scheduler.start();
            scheduler.stop();
            scheduler.start();
            scheduler.stop();
            std::_Exit(0);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST(HeartbeatSchedulerTest, EmptySchedulerDoesNotStart) {
    HeartbeatScheduler scheduler([](const HeartbeatJob &) {});
    scheduler.start();
    scheduler.stop();
}

TEST(HeartbeatSchedulerTest, StopIsIdempotent) {
    HeartbeatScheduler scheduler([](const HeartbeatJob &) {});
    scheduler.stop();
    scheduler.stop();
}

TEST(HeartbeatSchedulerTest, RunPendingInvokesCallbackOnMatch) {
    int fire_count = 0;

    HeartbeatScheduler scheduler([&](const HeartbeatJob &job) {
        EXPECT_EQ(job.name, "every-minute");
        ++fire_count;
    });

    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("every-minute", *expr, "default", "cli", "tick");

    scheduler.run_pending(std::chrono::system_clock::time_point{std::chrono::seconds{0}});

    EXPECT_EQ(fire_count, 1);
}

TEST(HeartbeatSchedulerTest, AddingJobWhileRunningWakesSchedulerPromptly) {
    wait_until_not_near_minute_boundary();

    std::mutex mtx;
    std::condition_variable cv;
    int fire_count = 0;

    HeartbeatScheduler scheduler([&](const HeartbeatJob &job) {
        if (job.name != "wake-test") {
            return;
        }

        {
            std::scoped_lock lock(mtx);
            ++fire_count;
        }
        cv.notify_all();
    });

    auto far_future = parse_cron("0 0 1 1 *");
    ASSERT_TRUE(far_future.has_value());
    scheduler.add_job("far-future", *far_future, "default", "cli", "sleep");
    scheduler.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto every_minute = parse_cron("* * * * *");
    ASSERT_TRUE(every_minute.has_value());
    scheduler.add_job("wake-test", *every_minute, "default", "cli", "wake");

    {
        std::unique_lock lock(mtx);
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return fire_count > 0;
        }));
    }

    scheduler.stop();
    EXPECT_EQ(fire_count, 1);
}

TEST(HeartbeatSchedulerTest, LateFireDoesNotSuppressNextMinuteRun) {
    std::vector<std::chrono::system_clock::time_point> fired_at;

    HeartbeatScheduler scheduler([&](const HeartbeatJob &) {
        fired_at.push_back(std::chrono::system_clock::now());
    });

    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("every-minute", *expr, "default", "cli", "tick");

    const auto late_in_minute = std::chrono::system_clock::time_point{std::chrono::seconds{59}};
    const auto next_minute = std::chrono::system_clock::time_point{std::chrono::minutes{1}};

    scheduler.run_pending(late_in_minute);
    scheduler.run_pending(next_minute);

    EXPECT_EQ(fired_at.size(), 2);
}

TEST(HeartbeatSchedulerTest, SchedulerOnlyFiresOncePerMinuteBucket) {
    int fire_count = 0;

    HeartbeatScheduler scheduler([&](const HeartbeatJob &) {
        ++fire_count;
    });

    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("every-minute", *expr, "default", "cli", "tick");

    const auto first_fire = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
    const auto second_check_same_minute = std::chrono::system_clock::time_point{std::chrono::seconds{59}};

    scheduler.run_pending(first_fire);
    scheduler.run_pending(second_check_same_minute);

    EXPECT_EQ(fire_count, 1);
}

TEST(HeartbeatSchedulerTest, DestructorStopsScheduler) {
    {
        HeartbeatScheduler scheduler([](const HeartbeatJob &) {});
        auto expr = parse_cron("* * * * *");
        ASSERT_TRUE(expr.has_value());
        scheduler.add_job("destruct-test", *expr, "default", "cli", "test");
        scheduler.start();
    }
    // If destructor doesn't stop, this would hang
}


// ── remove_job / has_job tests ──────────────────

TEST(HeartbeatSchedulerTest, HasJobReturnsTrueForExistingJob) {
    HeartbeatScheduler scheduler([](const HeartbeatJob &) {});
    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("my-job", *expr, "default", "cli", "test");
    EXPECT_TRUE(scheduler.has_job("my-job"));
    EXPECT_FALSE(scheduler.has_job("nonexistent"));
}

TEST(HeartbeatSchedulerTest, RemoveDynamicJobSucceeds) {
    HeartbeatScheduler scheduler([](const HeartbeatJob &) {});
    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("dyn-job", *expr, "default", "cli", "test", true);
    EXPECT_TRUE(scheduler.has_job("dyn-job"));
    EXPECT_TRUE(scheduler.remove_job("dyn-job"));
    EXPECT_FALSE(scheduler.has_job("dyn-job"));
}

TEST(HeartbeatSchedulerTest, RemoveStaticJobFails) {
    HeartbeatScheduler scheduler([](const HeartbeatJob &) {});
    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("static-job", *expr, "default", "cli", "test", false);
    EXPECT_FALSE(scheduler.remove_job("static-job"));
    EXPECT_TRUE(scheduler.has_job("static-job"));
}

TEST(HeartbeatSchedulerTest, RemoveNonexistentJobFails) {
    HeartbeatScheduler scheduler([](const HeartbeatJob &) {});
    EXPECT_FALSE(scheduler.remove_job("ghost"));
}

// ── Isolated session struct tests ───────────────

TEST(IsolatedSessionTest, InboundMessageDefaultsNotIsolated) {
    InboundMessage msg;
    EXPECT_FALSE(msg.isolated);
    EXPECT_FALSE(msg.light_context);
}

TEST(IsolatedSessionTest, IsolationFlagsSetCorrectly) {
    InboundMessage msg{.jid = "heartbeat:test", .content = "check", .isolated = true, .light_context = true};
    EXPECT_TRUE(msg.isolated);
    EXPECT_TRUE(msg.light_context);
}

TEST(HeartbeatSchedulerTest, FireJobInvokesCallback) {
    std::string fired_name;
    HeartbeatScheduler scheduler([&](const HeartbeatJob &job) {
        fired_name = job.name;
    });

    auto expr = parse_cron("0 0 1 1 *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("fire-test", *expr, "default", "cli", "test prompt");

    EXPECT_TRUE(scheduler.fire_job("fire-test"));
    EXPECT_EQ(fired_name, "fire-test");
}

TEST(HeartbeatSchedulerTest, FireNonexistentJobReturnsFalse) {
    HeartbeatScheduler scheduler([](const HeartbeatJob &) {});
    EXPECT_FALSE(scheduler.fire_job("nonexistent"));
}

// ── End-to-end smoke test ───────────────────────
// Tests the full pipeline: heartbeat fires → reads HEARTBEAT.md → prompt includes content → HEARTBEAT_OK detection works

TEST(HeartbeatE2ETest, FullPipelineHeartbeatMdToOkSuppression) {
    // 1. Create a temp HEARTBEAT.md
    const auto md_path = make_test_path("orangutan-test-e2e-heartbeat.md");
    ASSERT_TRUE(write_test_file(md_path, "- [ ] Check server health\n- [ ] Review logs\n"));

    // 2. Set up scheduler with HEARTBEAT.md path and capture the fired prompt
    std::string captured_prompt;
    HeartbeatScheduler scheduler([&](const HeartbeatJob &job) {
        captured_prompt = job.prompt;
    });
    scheduler.set_heartbeat_md_path(md_path.string());

    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("e2e-test", *expr, "default", "cli", "Run your heartbeat checks.");

    // 3. Fire to simulate a heartbeat run — run_pending prepends HEARTBEAT.md content
    scheduler.run_pending(std::chrono::system_clock::time_point{std::chrono::seconds{0}});

    // 4. Verify HEARTBEAT.md content was prepended to the prompt
    EXPECT_NE(captured_prompt.find("Check server health"), std::string::npos);
    EXPECT_NE(captured_prompt.find("Run your heartbeat checks."), std::string::npos);

    // 5. Simulate the agent responding with HEARTBEAT_OK — verify suppression
    EXPECT_TRUE(detect_heartbeat_ok("HEARTBEAT_OK All systems normal.", 300));
    EXPECT_TRUE(detect_heartbeat_ok("HEARTBEAT_OK", 300));

    // 6. Simulate a real issue — no suppression
    EXPECT_FALSE(detect_heartbeat_ok("WARNING: Server health check failed. CPU at 95%. Investigating.", 300));

    std::filesystem::remove(md_path);
}

TEST(HeartbeatE2ETest, EmptyHeartbeatMdSkipsRun) {
    const auto md_path = make_test_path("orangutan-test-e2e-empty.md");
    ASSERT_TRUE(write_test_file(md_path, "  \n\n  "));

    int fire_count = 0;
    HeartbeatScheduler scheduler([&](const HeartbeatJob &) {
        ++fire_count;
    });
    scheduler.set_heartbeat_md_path(md_path.string());

    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler.add_job("skip-test", *expr, "default", "cli", "should not fire");

    scheduler.run_pending(std::chrono::system_clock::time_point{std::chrono::seconds{0}});

    // Empty HEARTBEAT.md means skip — callback should not fire
    EXPECT_EQ(fire_count, 0);

    std::filesystem::remove(md_path);
}
