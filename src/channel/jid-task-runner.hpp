#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace orangutan::channel {

    class JidTaskRunner {
    public:
        using Task = std::move_only_function<void()>;

        class BlockingLease {
        public:
            BlockingLease() = default;
            ~BlockingLease();

            BlockingLease(const BlockingLease &) = delete;
            BlockingLease &operator=(const BlockingLease &) = delete;
            BlockingLease(BlockingLease &&other) noexcept;
            BlockingLease &operator=(BlockingLease &&other) noexcept;

        private:
            friend class JidTaskRunner;

            explicit BlockingLease(JidTaskRunner *runner);

            JidTaskRunner *runner_ = nullptr;
        };

        explicit JidTaskRunner(std::size_t worker_count);
        ~JidTaskRunner();

        JidTaskRunner(const JidTaskRunner &) = delete;
        JidTaskRunner &operator=(const JidTaskRunner &) = delete;
        JidTaskRunner(JidTaskRunner &&) = delete;
        JidTaskRunner &operator=(JidTaskRunner &&) = delete;

        void submit(std::string_view jid, Task task);
        void shutdown(bool discard_pending = false);

        [[nodiscard]]
        BlockingLease acquire_blocking_lease();

        [[nodiscard]]
        std::size_t worker_count() const;

    private:
        struct QueuedTask;
        struct SchedulerModel;

        struct Bucket {
            std::deque<std::unique_ptr<QueuedTask>> tasks;
            bool active = false;
        };

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::unordered_map<std::string, Bucket> buckets_;
        std::queue<std::string> ready_jids_;
        std::size_t base_worker_count_ = 0;
        std::size_t desired_worker_count_ = 0;
        std::size_t live_worker_count_ = 0;
        std::vector<std::thread> workers_;
        std::atomic<bool> stopping_{false};
        std::atomic<bool> discard_pending_{false};

        void enqueue_scheduled_task(std::string_view jid, std::unique_ptr<QueuedTask> task);
        void spawn_worker_locked();
        void release_blocking_lease();
        void worker_loop();
    };

} // namespace orangutan::channel

namespace orangutan {

    using channel::JidTaskRunner;

} // namespace orangutan
