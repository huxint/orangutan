// include/oran/async/channel.hpp — bounded coroutine channel.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>

namespace orangutan::async {

template <typename T>
class Channel {
public:
  Channel(asio::any_io_executor executor, std::size_t capacity)
      : state_(std::make_shared<State>(std::move(executor), capacity)) {}

  [[nodiscard]] std::size_t capacity() const noexcept {
    return state_->capacity();
  }
  [[nodiscard]] std::size_t size() const {
    return state_->size();
  }
  [[nodiscard]] bool closed() const {
    return state_->closed();
  }

  [[nodiscard]] Awaitable<core::Result<void>> send(T value) {
    auto cancellation = co_await asio::this_coro::cancellation_state;
    if (cancellation.cancelled() != asio::cancellation_type::none) {
      co_return std::unexpected(core::Error::cancelled());
    }
    co_return co_await async_send(std::move(value), asio::use_awaitable);
  }

  [[nodiscard]] core::Result<void> try_send(T value) {
    return state_->try_send(std::move(value));
  }

  [[nodiscard]] core::Result<std::optional<T>> try_receive() {
    return state_->try_receive();
  }

  [[nodiscard]] Awaitable<core::Result<T>> receive() {
    auto cancellation = co_await asio::this_coro::cancellation_state;
    if (cancellation.cancelled() != asio::cancellation_type::none) {
      co_return std::unexpected(core::Error::cancelled());
    }
    co_return co_await async_receive(asio::use_awaitable);
  }

  void close() noexcept {
    state_->close();
  }

private:
  using Deferred = std::move_only_function<void()>;
  using SendComplete = std::move_only_function<void(core::Result<void>)>;
  using ReceiveComplete = std::move_only_function<void(core::Result<T>)>;

  struct PendingSend {
    std::uint64_t id{};
    T value;
    SendComplete complete;
  };

  struct PendingReceive {
    std::uint64_t id{};
    ReceiveComplete complete;
  };

  class State : public std::enable_shared_from_this<State> {
  public:
    State(asio::any_io_executor executor, std::size_t capacity) : executor_(std::move(executor)), capacity_(capacity) {}

    [[nodiscard]] std::size_t capacity() const noexcept {
      return capacity_;
    }

    [[nodiscard]] std::size_t size() const {
      const std::scoped_lock lock{mutex_};
      return values_.size();
    }

    [[nodiscard]] bool closed() const {
      const std::scoped_lock lock{mutex_};
      return closed_;
    }

    template <typename Handler>
    void async_send(T value, Handler&& handler) {
      std::vector<Deferred> completions;
      const auto id = next_id();
      auto cancel_slot = asio::get_associated_cancellation_slot(handler);

      {
        const std::scoped_lock lock{mutex_};
        senders_.push_back(PendingSend{
            .id = id,
            .value = std::move(value),
            .complete = make_send_complete(std::forward<Handler>(handler)),
        });
        // Install the cancel handler after the waiter is in the queue, under
        // the same mutex. Installing earlier would leave a window where a
        // cancellation could fire, scan an empty queue, and be silently
        // dropped while the waiter is later pushed and never cancelled.
        // Mirrors the discipline in storage::Pool::async_acquire_writer.
        if (cancel_slot.is_connected()) {
          const std::weak_ptr<State> weak = this->shared_from_this();
          cancel_slot.assign([weak, id](asio::cancellation_type type) {
            if (type == asio::cancellation_type::none) {
              return;
            }
            if (auto state = weak.lock()) {
              state->cancel_send(id);
            }
          });
        }
        pump_locked(completions);
      }

      run_deferred(completions);
    }

    template <typename Handler>
    void async_receive(Handler&& handler) {
      std::vector<Deferred> completions;
      const auto id = next_id();
      auto cancel_slot = asio::get_associated_cancellation_slot(handler);

      {
        const std::scoped_lock lock{mutex_};
        receivers_.push_back(PendingReceive{
            .id = id,
            .complete = make_receive_complete(std::forward<Handler>(handler)),
        });
        if (cancel_slot.is_connected()) {
          const std::weak_ptr<State> weak = this->shared_from_this();
          cancel_slot.assign([weak, id](asio::cancellation_type type) {
            if (type == asio::cancellation_type::none) {
              return;
            }
            if (auto state = weak.lock()) {
              state->cancel_receive(id);
            }
          });
        }
        pump_locked(completions);
      }

      run_deferred(completions);
    }

    [[nodiscard]] core::Result<void> try_send(T value) {
      std::vector<Deferred> completions;
      core::Result<void> result{};

      {
        const std::scoped_lock lock{mutex_};
        pump_locked(completions);
        if (closed_) {
          result = std::unexpected(channel_closed_error());
        } else if (!receivers_.empty() && values_.empty()) {
          auto receiver = pop_front(receivers_);
          completions.push_back(complete_receive(std::move(receiver.complete), std::move(value)));
        } else if (values_.size() < capacity_) {
          values_.push_back(std::move(value));
          pump_locked(completions);
        } else {
          result = std::unexpected(core::Error{core::ErrorKind::mailbox_overflowed, "channel capacity exceeded"});
        }
      }

      run_deferred(completions);
      return result;
    }

    [[nodiscard]] core::Result<std::optional<T>> try_receive() {
      std::vector<Deferred> completions;
      core::Result<std::optional<T>> result{std::optional<T>{}};

      {
        const std::scoped_lock lock{mutex_};
        pump_locked(completions);
        if (!values_.empty()) {
          result = std::optional<T>{pop_front(values_)};
          pump_locked(completions);
        } else if (!senders_.empty()) {
          auto sender = pop_front(senders_);
          result = std::optional<T>{std::move(sender.value)};
          completions.push_back(complete_send(std::move(sender.complete), core::Result<void>{}));
        } else if (closed_) {
          result = std::unexpected(channel_closed_error());
        }
      }

      run_deferred(completions);
      return result;
    }

    void close() noexcept {
      std::vector<Deferred> completions;
      {
        const std::scoped_lock lock{mutex_};
        closed_ = true;
        pump_locked(completions);
      }
      run_deferred(completions);
    }

  private:
    [[nodiscard]] std::uint64_t next_id() noexcept {
      return next_id_.fetch_add(1, std::memory_order_relaxed);
    }

    // Capture the handler's associated executor before type-erasing it into
    // SendComplete / ReceiveComplete. Resuming on the channel's executor
    // instead would break asio's contract that a handler runs on
    // get_associated_executor(handler, default) — strand-bound coroutines
    // would resume off-strand and race against other work on the strand.
    template <typename Handler>
    [[nodiscard]] SendComplete make_send_complete(Handler&& handler) {
      auto completion_executor = asio::get_associated_executor(handler, executor_);
      return [completion_executor, handler = std::forward<Handler>(handler)](core::Result<void> result) mutable {
        asio::post(completion_executor, [handler = std::move(handler), result = std::move(result)]() mutable {
          std::move(handler)(std::move(result));
        });
      };
    }

    template <typename Handler>
    [[nodiscard]] ReceiveComplete make_receive_complete(Handler&& handler) {
      auto completion_executor = asio::get_associated_executor(handler, executor_);
      return [completion_executor, handler = std::forward<Handler>(handler)](core::Result<T> result) mutable {
        asio::post(completion_executor, [handler = std::move(handler), result = std::move(result)]() mutable {
          std::move(handler)(std::move(result));
        });
      };
    }

    static void run_deferred(std::vector<Deferred>& completions) {
      for (auto& completion : completions) {
        std::move(completion)();
      }
    }

    template <typename U>
    [[nodiscard]] static U pop_front(std::deque<U>& queue) {
      auto value = std::move(queue.front());
      queue.pop_front();
      return value;
    }

    [[nodiscard]] static core::Error channel_closed_error() {
      return core::Error{core::ErrorKind::cancelled, "channel closed"};
    }

    [[nodiscard]] static Deferred complete_send(SendComplete complete, core::Result<void> result) {
      return [complete = std::move(complete), result = std::move(result)]() mutable {
        complete(std::move(result));
      };
    }

    [[nodiscard]] static Deferred complete_receive(ReceiveComplete complete, core::Result<T> result) {
      return [complete = std::move(complete), result = std::move(result)]() mutable {
        complete(std::move(result));
      };
    }

    void pump_locked(std::vector<Deferred>& completions) {
      while (true) {
        while (!receivers_.empty() && !values_.empty()) {
          auto receiver = pop_front(receivers_);
          auto value = pop_front(values_);
          completions.push_back(complete_receive(std::move(receiver.complete), std::move(value)));
        }

        if (closed_) {
          while (!senders_.empty()) {
            auto sender = pop_front(senders_);
            completions.push_back(complete_send(std::move(sender.complete), std::unexpected(channel_closed_error())));
          }
          while (!receivers_.empty() && !values_.empty()) {
            auto receiver = pop_front(receivers_);
            auto value = pop_front(values_);
            completions.push_back(complete_receive(std::move(receiver.complete), std::move(value)));
          }
          while (!receivers_.empty()) {
            auto receiver = pop_front(receivers_);
            completions.push_back(
                complete_receive(std::move(receiver.complete), std::unexpected(channel_closed_error())));
          }
          return;
        }

        if (senders_.empty()) {
          return;
        }

        if (!receivers_.empty()) {
          auto sender = pop_front(senders_);
          auto receiver = pop_front(receivers_);
          completions.push_back(complete_receive(std::move(receiver.complete), std::move(sender.value)));
          completions.push_back(complete_send(std::move(sender.complete), core::Result<void>{}));
          continue;
        }

        if (values_.size() < capacity_) {
          auto sender = pop_front(senders_);
          values_.push_back(std::move(sender.value));
          completions.push_back(complete_send(std::move(sender.complete), core::Result<void>{}));
          continue;
        }

        return;
      }
    }

    void cancel_send(std::uint64_t id) {
      std::vector<Deferred> completions;
      {
        const std::scoped_lock lock{mutex_};
        for (auto it = senders_.begin(); it != senders_.end(); ++it) {
          if (it->id == id) {
            auto sender = std::move(*it);
            senders_.erase(it);
            completions.push_back(complete_send(std::move(sender.complete), std::unexpected(core::Error::cancelled())));
            break;
          }
        }
      }
      run_deferred(completions);
    }

    void cancel_receive(std::uint64_t id) {
      std::vector<Deferred> completions;
      {
        const std::scoped_lock lock{mutex_};
        for (auto it = receivers_.begin(); it != receivers_.end(); ++it) {
          if (it->id == id) {
            auto receiver = std::move(*it);
            receivers_.erase(it);
            completions.push_back(
                complete_receive(std::move(receiver.complete), std::unexpected(core::Error::cancelled())));
            break;
          }
        }
      }
      run_deferred(completions);
    }

    asio::any_io_executor executor_;
    std::size_t capacity_{};
    mutable std::mutex mutex_;
    bool closed_{false};
    std::atomic_uint64_t next_id_{1};
    std::deque<T> values_;
    std::deque<PendingSend> senders_;
    std::deque<PendingReceive> receivers_;
  };

  template <typename CompletionToken>
  auto async_send(T value, CompletionToken&& token) {
    return asio::async_initiate<CompletionToken, void(core::Result<void>)>(
        [state = state_, value = std::move(value)]<typename Handler>(Handler&& handler) mutable {
          state->async_send(std::move(value), std::forward<Handler>(handler));
        },
        token);
  }

  template <typename CompletionToken>
  auto async_receive(CompletionToken&& token) {
    return asio::async_initiate<CompletionToken, void(core::Result<T>)>(
        [state = state_]<typename Handler>(Handler&& handler) mutable {
          state->async_receive(std::forward<Handler>(handler));
        },
        token);
  }

  std::shared_ptr<State> state_;
};

}  // namespace orangutan::async
