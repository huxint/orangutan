#pragma once

#include "channel/channel.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace orangutan::channel {

    class MessageQueue {
    public:
        void push(InboundMessage msg) {
            {
                std::scoped_lock lock(mutex_);
                if (shutdown_) {
                    return;
                }
                queue_.push(std::move(msg));
            }
            cv_.notify_one();
        }

        InboundMessage pop() {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return shutdown_ || !queue_.empty();
            });

            if (queue_.empty()) {
                return {};
            }

            InboundMessage msg = std::move(queue_.front());
            queue_.pop();
            return msg;
        }

        bool try_pop(InboundMessage &msg, std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool ready = cv_.wait_for(lock, timeout, [this] {
                return shutdown_ || !queue_.empty();
            });
            if (!ready || queue_.empty()) {
                return false;
            }

            msg = std::move(queue_.front());
            queue_.pop();
            return true;
        }

        void shutdown() {
            {
                std::scoped_lock lock(mutex_);
                shutdown_ = true;
            }
            cv_.notify_all();
        }

        [[nodiscard]]
        bool is_shutdown() const {
            std::scoped_lock lock(mutex_);
            return shutdown_;
        }

    private:
        std::queue<InboundMessage> queue_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        bool shutdown_ = false;
    };

} // namespace orangutan::channel

namespace orangutan {

    using channel::MessageQueue;

} // namespace orangutan
