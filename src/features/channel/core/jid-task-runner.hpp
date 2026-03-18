#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace orangutan {

class JidTaskRunner {
public:
    using Task = std::function<void()>;

    explicit JidTaskRunner(size_t worker_count);
    ~JidTaskRunner();

    JidTaskRunner(const JidTaskRunner &) = delete;
    JidTaskRunner &operator=(const JidTaskRunner &) = delete;
    JidTaskRunner(JidTaskRunner &&) = delete;
    JidTaskRunner &operator=(JidTaskRunner &&) = delete;

    void submit(const std::string &jid, Task task);
    void shutdown(bool discard_pending = false);

    [[nodiscard]]
    size_t worker_count() const;

private:
    struct Bucket {
        std::deque<Task> tasks;
        bool active = false;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Bucket> buckets_;
    std::queue<std::string> ready_jids_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> discard_pending_{false};

    void worker_loop();
};

} // namespace orangutan
