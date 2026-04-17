#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
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
            explicit BlockingLease(JidTaskRunner *runner);
            ~BlockingLease();

            BlockingLease(const BlockingLease &) = delete;
            BlockingLease &operator=(const BlockingLease &) = delete;
            BlockingLease(BlockingLease &&) noexcept;
            BlockingLease &operator=(BlockingLease &&) noexcept;

        private:
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

        /// Hint that the calling task is about to block (e.g. awaiting an
        /// approval). Each active lease provisions extra shared-pool capacity
        /// so unrelated jids can continue to make progress while the caller
        /// waits.
        [[nodiscard]]
        BlockingLease acquire_blocking_lease();

        [[nodiscard]]
        std::size_t worker_count() const;

    private:
        struct QueuedTask;
        struct SchedulerModel;

        struct Bucket {
            std::deque<std::unique_ptr<QueuedTask>> tasks;
        };

        struct Impl;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, Bucket> buckets_;
        std::queue<std::string> ready_jids_;
        std::atomic<bool> stopping_{false};
        std::atomic<bool> discard_pending_{false};
        std::size_t base_worker_count_ = 0;
        std::size_t blocking_leases_ = 0;
        std::size_t active_drainers_ = 0;
        std::size_t worker_count_ = 0;
        std::unique_ptr<Impl> impl_;

        void enqueue_scheduled_task(std::string_view jid, std::unique_ptr<QueuedTask> task);
        void schedule_drain_slot(std::size_t slot_index);
        void drain_ready_tasks();
        void release_blocking_lease();
    };

} // namespace orangutan::channel

namespace orangutan {

    using channel::JidTaskRunner;

} // namespace orangutan
