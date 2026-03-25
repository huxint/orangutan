#include "features/channel/core/jid-task-runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace orangutan;

namespace {

TEST_CASE("same_jid_tasks_run_sequentially_in_submission_order") {
    JidTaskRunner runner(2);

    std::mutex events_mutex;
    std::vector<int> events;
    std::promise<void> done_promise;
    std::atomic<int> remaining = 2;
    auto done_future = done_promise.get_future();

    auto mark_done = [&] {
        if (remaining.fetch_sub(1) == 1) {
            done_promise.set_value();
        }
    };

    runner.submit("qqbot:c2c:alice", [&] {
        {
            std::scoped_lock lock(events_mutex);
            events.push_back(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        {
            std::scoped_lock lock(events_mutex);
            events.push_back(2);
        }
        mark_done();
    });

    runner.submit("qqbot:c2c:alice", [&] {
        {
            std::scoped_lock lock(events_mutex);
            events.push_back(3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        {
            std::scoped_lock lock(events_mutex);
            events.push_back(4);
        }
        mark_done();
    });

    REQUIRE(done_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(events == std::vector<int>{1, 2, 3, 4});

    runner.shutdown();
};

TEST_CASE("different_jids_can_run_in_parallel") {
    JidTaskRunner runner(2);

    std::atomic<int> active = 0;
    int max_active = 0;
    std::mutex max_mutex;
    std::promise<void> started_a;
    std::promise<void> started_b;
    std::promise<void> release;
    auto started_a_future = started_a.get_future();
    auto started_b_future = started_b.get_future();
    auto release_future = release.get_future().share();

    auto task = [&](std::promise<void> &started) {
        const int current = active.fetch_add(1) + 1;
        {
            std::scoped_lock lock(max_mutex);
            max_active = std::max(max_active, current);
        }
        started.set_value();
        release_future.wait();
        active.fetch_sub(1);
    };

    runner.submit("qqbot:c2c:alice", [&] {
        task(started_a);
    });
    runner.submit("qqbot:c2c:bob", [&] {
        task(started_b);
    });

    REQUIRE(started_a_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE(started_b_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(max_active >= 2);

    release.set_value();
    runner.shutdown();
};

TEST_CASE("shutdown_can_discard_pending_tasks") {
    JidTaskRunner runner(1);

    std::promise<void> first_started;
    std::promise<void> release_first;
    std::promise<void> shutdown_started;
    std::atomic<bool> ran_second = false;

    runner.submit("qqbot:c2c:alice", [&] {
        first_started.set_value();
        release_first.get_future().wait();
    });
    runner.submit("qqbot:c2c:alice", [&] {
        ran_second.store(true);
    });

    auto started_future = first_started.get_future();
    REQUIRE(started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    auto shutdown_future = std::async(std::launch::async, [&runner, &shutdown_started] {
        shutdown_started.set_value();
        runner.shutdown(true);
    });
    auto shutdown_started_future = shutdown_started.get_future();
    REQUIRE(shutdown_started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    release_first.set_value();
    REQUIRE(shutdown_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK_FALSE(ran_second.load());
};

TEST_CASE("blocking_lease_allows_other_jids_to_run_with_single_worker") {
    JidTaskRunner runner(1);

    std::promise<void> alice_waiting;
    std::promise<void> bob_started;
    std::promise<void> release_alice;
    auto alice_waiting_future = alice_waiting.get_future();
    auto bob_started_future = bob_started.get_future();
    auto release_alice_future = release_alice.get_future().share();

    runner.submit("qqbot:c2c:alice", [&] {
        auto blocking_lease = runner.acquire_blocking_lease();
        alice_waiting.set_value();
        release_alice_future.wait();
    });

    runner.submit("qqbot:c2c:bob", [&] {
        bob_started.set_value();
    });

    REQUIRE(alice_waiting_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE(bob_started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    release_alice.set_value();
    runner.shutdown();
};

TEST_CASE("task_exceptions_do_not_terminate_runner_or_block_following_tasks") {
    JidTaskRunner runner(1);

    std::promise<void> second_ran;
    auto second_ran_future = second_ran.get_future();

    runner.submit("qqbot:c2c:alice", [] {
        throw std::runtime_error("boom");
    });
    runner.submit("qqbot:c2c:alice", [&] {
        second_ran.set_value();
    });

    REQUIRE(second_ran_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    runner.shutdown();
};

TEST_CASE("shutdown_discard_releases_pending_sender_operations") {
    JidTaskRunner runner(1);

    std::promise<void> first_started;
    std::promise<void> release_first;
    auto release_first_future = release_first.get_future().share();

    runner.submit("qqbot:c2c:alice", [&] {
        first_started.set_value();
        release_first_future.wait();
    });

    auto pending_capture = std::make_shared<int>(42);
    std::weak_ptr<int> pending_capture_weak = pending_capture;
    runner.submit("qqbot:c2c:alice", [pending_capture] {
        static_cast<void>(pending_capture);
    });
    pending_capture.reset();

    REQUIRE(first_started.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    auto shutdown_future = std::async(std::launch::async, [&runner] {
        runner.shutdown(true);
    });

    release_first.set_value();
    REQUIRE(shutdown_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(pending_capture_weak.expired());
};

} // namespace
