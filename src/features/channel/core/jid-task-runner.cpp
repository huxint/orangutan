#include "features/channel/core/jid-task-runner.hpp"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan {

JidTaskRunner::BlockingLease::BlockingLease(JidTaskRunner *runner)
: runner_(runner) {}

JidTaskRunner::BlockingLease::~BlockingLease() {
    if (runner_ != nullptr) {
        runner_->release_blocking_lease();
    }
}

JidTaskRunner::BlockingLease::BlockingLease(BlockingLease &&other) noexcept
: runner_(std::exchange(other.runner_, nullptr)) {}

JidTaskRunner::BlockingLease &JidTaskRunner::BlockingLease::operator=(BlockingLease &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (runner_ != nullptr) {
        runner_->release_blocking_lease();
    }
    runner_ = std::exchange(other.runner_, nullptr);
    return *this;
}

JidTaskRunner::JidTaskRunner(size_t worker_count)
: base_worker_count_(worker_count),
  desired_worker_count_(worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("JidTaskRunner requires at least one worker");
    }

    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        spawn_worker_locked();
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

JidTaskRunner::BlockingLease JidTaskRunner::acquire_blocking_lease() {
    std::scoped_lock lock(mutex_);
    if (stopping_.load()) {
        return {};
    }

    ++desired_worker_count_;
    if (live_worker_count_ < desired_worker_count_) {
        spawn_worker_locked();
    }
    cv_.notify_one();
    return BlockingLease(this);
}

size_t JidTaskRunner::worker_count() const {
    return base_worker_count_;
}

void JidTaskRunner::spawn_worker_locked() {
    ++live_worker_count_;
    workers_.emplace_back([this] {
        worker_loop();
    });
}

void JidTaskRunner::release_blocking_lease() {
    std::scoped_lock lock(mutex_);
    if (desired_worker_count_ > base_worker_count_) {
        --desired_worker_count_;
    }
    cv_.notify_all();
}

void JidTaskRunner::worker_loop() {
    while (true) {
        std::string jid;
        Task task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_.load() || !ready_jids_.empty() || live_worker_count_ > desired_worker_count_;
            });

            if (ready_jids_.empty()) {
                if (stopping_.load() || live_worker_count_ > desired_worker_count_) {
                    --live_worker_count_;
                    cv_.notify_all();
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
