#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <exec/static_thread_pool.hpp>
#include <mutex>
#include <span>
#include <stdexec/execution.hpp>
#include <thread>
#include <type_traits>
#include <vector>

namespace orangutan::utils {

    /// Lazily-initialised process-wide pool used by simple parallel helpers.
    /// Kept separate from TaskPool so tools doing short fan-out work do not
    /// contend with long-lived subsystem schedulers.
    [[nodiscard]]
    inline exec::static_thread_pool &shared_io_pool() {
        static exec::static_thread_pool pool{std::max<std::uint32_t>(2U, std::thread::hardware_concurrency())};
        return pool;
    }

    /// Apply `fn` to each element of `items` in parallel on the shared IO pool,
    /// returning results in input order. If any invocation throws, the first
    /// exception is rethrown after all tasks finish.
    template <typename T, typename Fn>
    auto parallel_map(std::span<const T> items, Fn fn) -> std::vector<std::invoke_result_t<Fn, const T &>> {
        using R = std::invoke_result_t<Fn, const T &>;
        std::vector<R> results(items.size());

        if (items.empty()) {
            return results;
        }
        if (items.size() == 1) {
            results[0] = fn(items[0]);
            return results;
        }

        auto scheduler = shared_io_pool().get_scheduler();

        std::atomic<std::size_t> remaining{items.size()};
        std::mutex mx;
        std::condition_variable cv;
        std::exception_ptr first_error;

        for (std::size_t i = 0; i < items.size(); ++i) {
            stdexec::start_detached(stdexec::schedule(scheduler) | stdexec::then([i, &items, &fn, &results, &remaining, &mx, &cv, &first_error] {
                                        try {
                                            results[i] = fn(items[i]);
                                        } catch (...) {
                                            std::lock_guard lock{mx};
                                            if (!first_error) {
                                                first_error = std::current_exception();
                                            }
                                        }
                                        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                            std::lock_guard lock{mx};
                                            cv.notify_one();
                                        }
                                    }));
        }

        std::unique_lock lock{mx};
        cv.wait(lock, [&remaining] { return remaining.load(std::memory_order_acquire) == 0; });
        if (first_error) {
            std::rethrow_exception(first_error);
        }
        return results;
    }

} // namespace orangutan::utils
