#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace orangutan::utils {
    class TaskPool;
}

namespace orangutan::channel {

    class JidTaskRunner {
    public:
        using Task = std::move_only_function<void()>;

        class BlockingLease {
        public:
            BlockingLease() = default;
            ~BlockingLease() = default;

            BlockingLease(const BlockingLease &) = delete;
            BlockingLease &operator=(const BlockingLease &) = delete;
            BlockingLease(BlockingLease &&) noexcept = default;
            BlockingLease &operator=(BlockingLease &&) noexcept = default;
        };

        explicit JidTaskRunner(std::size_t worker_count);
        ~JidTaskRunner();

        JidTaskRunner(const JidTaskRunner &) = delete;
        JidTaskRunner &operator=(const JidTaskRunner &) = delete;
        JidTaskRunner(JidTaskRunner &&) = delete;
        JidTaskRunner &operator=(JidTaskRunner &&) = delete;

        void submit(std::string_view jid, Task task);
        void shutdown(bool discard_pending = false);

        /// Hint that the calling task is about to block (e.g. awaiting an
        /// approval). The lease itself is a no-op now that JidTaskRunner is
        /// backed by a shared TaskPool: each blocking task occupies one pool
        /// worker while other jids continue to run on the remaining workers.
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

        struct Impl;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, Bucket> buckets_;
        std::atomic<bool> stopping_{false};
        std::atomic<bool> discard_pending_{false};
        std::size_t worker_count_ = 0;
        std::unique_ptr<Impl> impl_;

        void enqueue_scheduled_task(std::string_view jid, std::unique_ptr<QueuedTask> task);
        void schedule_drain(std::string jid);
        void drain_step(const std::string &jid);
    };

} // namespace orangutan::channel

namespace orangutan {

    using channel::JidTaskRunner;

} // namespace orangutan
