#include "features/channel/core/jid-task-runner.hpp"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan {

JidTaskRunner::JidTaskRunner(size_t worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("JidTaskRunner requires at least one worker");
    }

    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] {
            worker_loop();
        });
    }
}

JidTaskRunner::~JidTaskRunner() {
    shutdown(true);
}

void JidTaskRunner::submit(const std::string &jid, Task task) {
    if (jid.empty()) {
        throw std::invalid_argument("JidTaskRunner requires a non-empty jid");
    }

    std::scoped_lock lock(mutex_);
    if (stopping_.load()) {
        return;
    }

    auto &bucket = buckets_[jid];
    bucket.tasks.push_back(std::move(task));
    if (bucket.active) {
        return;
    }

    bucket.active = true;
    ready_jids_.push(jid);
    cv_.notify_one();
}

void JidTaskRunner::shutdown(bool discard_pending) {
    {
        discard_pending_.store(discard_pending);
        stopping_.store(true);

        std::scoped_lock lock(mutex_);
        if (workers_.empty()) {
            return;
        }

        if (discard_pending_.load()) {
            ready_jids_ = {};
            for (auto it = buckets_.begin(); it != buckets_.end();) {
                it->second.tasks.clear();
                if (!it->second.active) {
                    it = buckets_.erase(it);
                    continue;
                }
                ++it;
            }
        }
    }

    cv_.notify_all();
    for (auto &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

size_t JidTaskRunner::worker_count() const {
    return workers_.size();
}

void JidTaskRunner::worker_loop() {
    while (true) {
        std::string jid;
        Task task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_.load() || !ready_jids_.empty();
            });

            if (ready_jids_.empty()) {
                if (stopping_.load()) {
                    return;
                }
                continue;
            }

            jid = std::move(ready_jids_.front());
            ready_jids_.pop();

            auto it = buckets_.find(jid);
            if (it == buckets_.end() || it->second.tasks.empty()) {
                continue;
            }

            task = std::move(it->second.tasks.front());
            it->second.tasks.pop_front();
        }

        try {
            task();
        } catch (const std::exception &e) {
            spdlog::error("Unhandled exception in JidTaskRunner task for '{}': {}", jid, e.what());
        } catch (...) {
            spdlog::error("Unhandled non-standard exception in JidTaskRunner task for '{}'", jid);
        }

        {
            std::scoped_lock lock(mutex_);
            auto it = buckets_.find(jid);
            if (it == buckets_.end()) {
                continue;
            }

            if (discard_pending_.load()) {
                it->second.tasks.clear();
            }

            if (it->second.tasks.empty()) {
                it->second.active = false;
                buckets_.erase(it);
                continue;
            }

            ready_jids_.push(jid);
            cv_.notify_one();
        }
    }
}

} // namespace orangutan
