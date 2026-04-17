#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::web {

    /// A single broadcastable event on the global bus. Every event has a monotonically
    /// increasing sequence id so clients can resume after disconnect using `Last-Event-ID`.
    struct BusEvent {
        std::uint64_t sequence = 0;
        std::string kind;  // e.g. "chat.text", "agent.started", "automation.run", "system.status"
        std::string scope; // optional grouping token (agent_key, session_id, automation_id)
        nlohmann::json payload = nlohmann::json::object();
        std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    };

    /// Thread-safe publish/subscribe hub used by the observatory event stream.
    /// Retains a small ring buffer of recent events so late subscribers (or reconnecting
    /// clients with a Last-Event-ID) can catch up without missing anything.
    class EventBus {
    public:
        using Handler = std::function<void(const BusEvent &)>;

        static constexpr std::size_t DEFAULT_HISTORY = 256;

        explicit EventBus(std::size_t history_size = DEFAULT_HISTORY) : history_capacity_(history_size) {}
        ~EventBus() = default;

        EventBus(const EventBus &) = delete;
        EventBus &operator=(const EventBus &) = delete;
        EventBus(EventBus &&) = delete;
        EventBus &operator=(EventBus &&) = delete;

        /// RAII subscription handle — destruction unregisters the handler.
        class Subscription {
        public:
            Subscription() = default;
            Subscription(EventBus *bus, std::uint64_t id) : bus_(bus), id_(id) {}
            ~Subscription() {
                reset();
            }

            Subscription(const Subscription &) = delete;
            Subscription &operator=(const Subscription &) = delete;
            Subscription(Subscription &&other) noexcept : bus_(other.bus_), id_(other.id_) {
                other.bus_ = nullptr;
                other.id_ = 0;
            }
            Subscription &operator=(Subscription &&other) noexcept {
                if (this != &other) {
                    reset();
                    bus_ = other.bus_;
                    id_ = other.id_;
                    other.bus_ = nullptr;
                    other.id_ = 0;
                }
                return *this;
            }

            void reset() noexcept {
                if (bus_ != nullptr && id_ != 0) {
                    bus_->unsubscribe(id_);
                }
                bus_ = nullptr;
                id_ = 0;
            }

        private:
            EventBus *bus_ = nullptr;
            std::uint64_t id_ = 0;
        };

        /// Publish an event to every live subscriber.
        void publish(std::string_view kind, std::string_view scope, nlohmann::json payload) {
            BusEvent event{
                .sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1,
                .kind = std::string(kind),
                .scope = std::string(scope),
                .payload = std::move(payload),
                .timestamp = std::chrono::system_clock::now(),
            };

            std::vector<Handler> handlers;
            {
                std::scoped_lock lock(mutex_);
                history_.push_back(event);
                if (history_.size() > history_capacity_) {
                    history_.pop_front();
                }
                handlers.reserve(entries_.size());
                for (const auto &entry : entries_) {
                    handlers.push_back(entry.handler);
                }
            }
            for (const auto &handler : handlers) {
                handler(event);
            }
        }

        /// Register a callback. Replays any retained events with `sequence > replay_after`
        /// so a reconnecting client (with `Last-Event-ID`) rejoins without data loss.
        [[nodiscard]]
        Subscription subscribe(Handler handler, std::uint64_t replay_after = 0) {
            const auto id = next_subscriber_id_.fetch_add(1, std::memory_order_relaxed) + 1;
            std::vector<BusEvent> replay;
            {
                std::scoped_lock lock(mutex_);
                entries_.push_back(Entry{.id = id, .handler = std::move(handler)});
                for (const auto &event : history_) {
                    if (event.sequence > replay_after) {
                        replay.push_back(event);
                    }
                }
            }
            const auto &replay_handler = entries_.back().handler;
            for (const auto &event : replay) {
                replay_handler(event);
            }
            return {this, id};
        }

        [[nodiscard]]
        std::uint64_t current_sequence() const noexcept {
            return next_sequence_.load(std::memory_order_relaxed);
        }

    private:
        struct Entry {
            std::uint64_t id = 0;
            Handler handler;
        };

        void unsubscribe(std::uint64_t id) noexcept {
            std::scoped_lock lock(mutex_);
            std::erase_if(entries_, [id](const Entry &e) {
                return e.id == id;
            });
        }

        mutable std::mutex mutex_;
        std::deque<BusEvent> history_;
        std::vector<Entry> entries_;
        std::size_t history_capacity_;
        std::atomic<std::uint64_t> next_sequence_{0};
        std::atomic<std::uint64_t> next_subscriber_id_{0};
    };

} // namespace orangutan::web
