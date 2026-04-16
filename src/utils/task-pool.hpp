#pragma once

#include <exec/static_thread_pool.hpp>

#include <cstddef>

namespace orangutan::utils {

    /// Shared thread pool backing the project's stdexec senders.
    ///
    /// Orangutan's CLAUDE.md mandates using stdexec's built-in pool instead
    /// of hand-rolled `std::thread` workers. This class owns one process-wide
    /// pool and hands out schedulers to subsystems that need async work —
    /// automation timers, QQ heartbeat loops, background retries, etc.
    ///
    /// Construction seeds the pool with `hardware_concurrency` threads when
    /// `thread_count` is zero; pass a positive number for a custom size.
    class TaskPool {
    public:
        TaskPool()
        : pool_{} {}

        explicit TaskPool(std::size_t thread_count)
        : pool_{static_cast<std::uint32_t>(thread_count)} {}

        ~TaskPool() {
            pool_.request_stop();
        }

        TaskPool(const TaskPool &) = delete;
        TaskPool &operator=(const TaskPool &) = delete;
        TaskPool(TaskPool &&) = delete;
        TaskPool &operator=(TaskPool &&) = delete;

        [[nodiscard]]
        auto scheduler() noexcept -> exec::static_thread_pool::scheduler {
            return pool_.get_scheduler();
        }

        [[nodiscard]]
        std::size_t thread_count() const noexcept {
            return pool_.available_parallelism();
        }

        void stop() noexcept {
            pool_.request_stop();
        }

    private:
        exec::static_thread_pool pool_;
    };

} // namespace orangutan::utils

namespace orangutan {

    using utils::TaskPool;

} // namespace orangutan
