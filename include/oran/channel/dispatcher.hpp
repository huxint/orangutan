#pragma once

#include <functional>
#include <memory>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/channel/adapter.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::hook {
class Bus;
}

namespace orangutan::channel {

using Transport = std::function<async::Awaitable<core::Result<Response>>(Conversation, Request)>;
using RunTurn = std::function<async::Awaitable<core::Result<std::string>>(Message)>;
/// Pure optional host formatting. The returned request is authorized before IO.
using RenderReply = std::function<core::Result<Request>(const Message&, std::string_view, std::size_t)>;

struct DispatcherOptions {
  permission::RuleSet rules;
  permission::Mode mode{permission::Mode::strict};
  hook::Bus* hooks{nullptr};
  std::size_t capacity{128};
  std::size_t max_reply_bytes{65536};
  std::chrono::milliseconds typing_interval{4000};
  std::chrono::milliseconds typing_ttl{60000};
  RenderReply render_reply{};
  /// Pure host splitting; each nonempty UTF-8 part must fit the adapter limit.
  std::function<core::Result<std::vector<std::string>>(std::string_view, std::size_t)> split_reply{};
};

struct Delivery {
  std::size_t parts_sent{0};
  bool duplicate{false};
  /// Advisory errors never replace an agent or delivery error.
  std::size_t activity_failures{0};
};

struct PendingReply {
  std::vector<std::string> parts;
  std::size_t next_part{0};
};

/// Bounded account/conversation coordinator. All calls use the same host strand.
/// Transport and runner must be cancel-aware and finish borrowed work on return.
/// Await every handle before destroying this dispatcher or its borrowed adapter/bus.
class Dispatcher {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<Dispatcher>>
  create(const Adapter& adapter, Transport transport, RunTurn run, DispatcherOptions options);
  class PrivateTag {
    PrivateTag() = default;
    friend class Dispatcher;
  };
  struct Impl;
  Dispatcher(std::unique_ptr<Impl> impl, PrivateTag);
  ~Dispatcher();
  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;
  [[nodiscard]] async::Awaitable<core::Result<Delivery>> handle(Message message);
  /// Failed/ambiguous deliveries are never automatically replayed or evicted.
  [[nodiscard]] core::Result<PendingReply> pending(const Message& message) const;
  /// Explicit host recovery after reconciling whether the failed send took effect.
  /// next_part is the first unsent part, including any send confirmed externally.
  [[nodiscard]] async::Awaitable<core::Result<Delivery>> resume(Message message, std::size_t next_part);
  /// Retire a terminal failure only after the host has durably handled it.
  [[nodiscard]] core::Result<void> acknowledge_failure(const Message& message);

private:
  [[nodiscard]] async::Awaitable<core::Result<Delivery>> handle_impl(Message message);
  [[nodiscard]] async::Awaitable<core::Result<Delivery>> resume_impl(Message message, std::size_t next_part);
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::channel
