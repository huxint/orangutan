#include "protocol.hpp"
#include <oran/channel/dispatcher.hpp>

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <oran/async/task_group.hpp>
#include <oran/hook/bus.hpp>

namespace orangutan::channel {
namespace {
using core::Error;
using core::Result;
std::string event_key(const Message& m) {
  return conversation_key(m.conversation) + ":" + m.event_id;
}
Error safe_error(const Error& error) {
  Error safe{error.kind(), "channel operation failed"};
  if (error.retry_after())
    safe.with_retry_after(*error.retry_after());
  return safe;
}
struct ConversationLease {
  std::unordered_set<std::string>& busy;
  const std::string& key;

  ConversationLease(std::unordered_set<std::string>& conversations, const std::string& conversation)
      : busy{conversations}, key{conversation} {
    busy.insert(key);
  }
  ~ConversationLease() {
    busy.erase(key);
  }
  ConversationLease(const ConversationLease&) = delete;
  ConversationLease& operator=(const ConversationLease&) = delete;
};
struct Entry {
  Message message;
  PendingReply reply{};
  bool generated{false};
  bool complete{false};
  std::optional<Error> error{};
  Delivery delivery{};
};
struct ActivityState {
  explicit ActivityState(asio::any_io_executor executor) : timer{std::move(executor)} {}
  asio::steady_timer timer;
  bool stopped{false};
  std::optional<Receipt> receipt;
  std::size_t failures{0};
};
}  // namespace

struct Dispatcher::Impl {
  const Adapter& adapter;
  Transport transport;
  RunTurn run;
  DispatcherOptions options;
  std::unordered_map<std::string, Entry> entries;
  std::deque<std::string> completed;
  std::unordered_set<std::string> busy;

  Result<Request> reply(const Message& message, std::string_view text, std::size_t part) const {
    return options.render_reply ? options.render_reply(message, text, part) : adapter.reply(message, text, part);
  }

  async::Awaitable<Result<void>> authorize(const Message& m, std::string operation, const Request* request) {
    const auto state = co_await asio::this_coro::cancellation_state;
    if (state.cancelled() != asio::cancellation_type::none)
      co_return std::unexpected(Error::cancelled());
    detail::Json input{{"conversation", conversation_key(m.conversation)}, {"sender", m.sender}};
    if (request) {
      input["method"] = request->method;
      input["path"] = request->path;
      input["body"] = request->body;
    }
    const std::vector<core::Capability> required =
        request ? std::vector{core::Capability::egress_http} : std::vector<core::Capability>{};
    const auto decision = permission::evaluate(options.rules, operation, input.dump(), required, options.mode);
    co_await options.hooks->publish_advisory(
        hook::Event::channel_action,
        hook::ChannelActionPayload{std::string{core::enum_name(adapter.platform())},
                                   m.conversation.account,
                                   std::move(operation),
                                   decision.verdict == permission::Verdict::allow});
    if (decision.verdict != permission::Verdict::allow)
      co_return std::unexpected(Error::permission_denied("channel operation requires an explicit allow decision"));
    co_return Result<void>{};
  }

  async::Awaitable<Result<Receipt>> send(const Message& m, Request request, std::string operation) {
    try {
      const bool needs_receipt = operation == "ChannelSend";
      auto allowed = co_await authorize(m, std::move(operation), &request);
      if (!allowed)
        co_return std::unexpected(allowed.error());
      auto response = co_await transport(m.conversation, std::move(request));
      if (!response)
        co_return std::unexpected(safe_error(response.error()));
      auto receipt = adapter.accept(*response);
      if (receipt && needs_receipt && receipt->id.empty())
        co_return std::unexpected(Error::parsing("channel send returned no message receipt"));
      co_return receipt;
    } catch (...) {
      // Inspect cancellation outside the handler (coroutine suspension is forbidden here).
    }
    auto state = co_await asio::this_coro::cancellation_state;
    co_return std::unexpected(state.cancelled() != asio::cancellation_type::none
                                  ? Error::cancelled()
                                  : Error::internal("channel transport failed"));
  }

  async::Awaitable<Result<void>> activity(Message m, ActivityState& state) {
    const auto deadline = std::chrono::steady_clock::now() + options.typing_ttl;
    const bool persistent = adapter.capabilities(m.conversation.kind).activity != Activity::renewable;
    unsigned consecutive_failures = 0;
    while (!state.stopped && std::chrono::steady_clock::now() < deadline) {
      auto request = adapter.activity(m, std::nullopt);
      if (!request || !*request) {
        if (!request)
          ++state.failures;
        break;
      }
      auto receipt = co_await send(m, std::move(**request), "ChannelTyping");
      if (!receipt) {
        ++state.failures;
        if (++consecutive_failures >= 2 || receipt.error().kind() == core::ErrorKind::rate_limit ||
            receipt.error().kind() == core::ErrorKind::permission_denied)
          break;
      } else {
        consecutive_failures = 0;
        state.receipt = std::move(*receipt);
        if (persistent) {
          if (adapter.capabilities(m.conversation.kind).activity == Activity::reaction && state.receipt->id.empty()) {
            ++state.failures;
            break;
          }
          // Reactions and expiring status need no renewal within the TTL.
          state.timer.expires_at(deadline);
        }
      }
      if (state.stopped)
        break;
      if (!persistent || !state.receipt)
        state.timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + options.typing_interval));
      asio::error_code ignored;
      co_await state.timer.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
      if (persistent && state.receipt)
        break;
    }
    if (state.receipt) {
      auto stop = adapter.activity(m, state.receipt);
      if (!stop)
        ++state.failures;
      else if (*stop) {
        auto removed = co_await send(m, std::move(**stop), "ChannelTyping");
        if (!removed)
          ++state.failures;
      }
    }
    co_return Result<void>{};
  }

  async::Awaitable<Result<Delivery>> deliver(Entry& entry) {
    // Preflight every part before any send (including QQ's passive-reply limit).
    for (std::size_t i = entry.reply.next_part; i < entry.reply.parts.size(); ++i) {
      auto request = reply(entry.message, entry.reply.parts[i], i);
      if (!request)
        co_return std::unexpected(request.error());
    }
    for (; entry.reply.next_part < entry.reply.parts.size(); ++entry.reply.next_part) {
      const auto part = entry.reply.next_part;
      auto request = reply(entry.message, entry.reply.parts[part], part);
      if (!request)
        co_return std::unexpected(request.error());
      auto receipt = co_await send(entry.message, std::move(*request), "ChannelSend");
      if (!receipt)
        co_return std::unexpected(receipt.error().with("confirmed_parts", std::to_string(part)));
      ++entry.delivery.parts_sent;
    }
    entry.complete = true;
    entry.error.reset();
    completed.push_back(event_key(entry.message));
    co_return entry.delivery;
  }

  async::Awaitable<Result<Delivery>> process(Entry& entry) {
    auto executor = co_await asio::this_coro::executor;
    ActivityState status{executor};
    auto tasks = async::TaskGroup::create(executor, {.max_tasks = 1, .max_completed = 1});
    if (!tasks)
      co_return std::unexpected(tasks.error());
    if (adapter.capabilities(entry.message.conversation.kind).activity != Activity::none) {
      auto spawned =
          tasks->spawn("channel-activity", [this, &entry, &status] { return activity(entry.message, status); });
      if (!spawned)
        co_return std::unexpected(spawned.error());
    }
    Result<std::string> answer = std::unexpected(Error::internal("channel turn failed"));
    try {
      answer = co_await run(entry.message);
    } catch (...) { /* Translate callback exceptions after joining status. */
    }
    bool cancelled = (co_await asio::this_coro::cancellation_state).cancelled() != asio::cancellation_type::none;
    // Record cancellation arriving during cleanup without forwarding it into
    // TaskGroup::join, which would otherwise cancel an in-flight status start.
    co_await asio::this_coro::reset_cancellation_state([&cancelled](asio::cancellation_type_t type) {
      cancelled |= type != asio::cancellation_type::none;
      return asio::cancellation_type::none;
    });
    co_await asio::this_coro::throw_if_cancelled(false);
    status.stopped = true;
    status.timer.cancel();
    auto joined = co_await tasks->join();
    entry.delivery.activity_failures = status.failures;
    // Remove the filter borrowing our local state before any return.
    co_await asio::this_coro::reset_cancellation_state(asio::enable_terminal_cancellation());
    if (!joined)
      co_return std::unexpected(joined.error());
    entry.delivery.activity_failures += joined->failed();
    if (!answer)
      co_return std::unexpected(cancelled ? Error::cancelled() : safe_error(answer.error()));
    if (answer->size() > options.max_reply_bytes)
      co_return std::unexpected(Error::invalid_argument("channel reply exceeds byte limit"));
    const auto limit = adapter.capabilities(entry.message.conversation.kind).text_bytes;
    auto parts = options.split_reply ? options.split_reply(*answer, limit) : split_text(*answer, limit);
    if (!parts)
      co_return std::unexpected(parts.error());
    if ((!answer->empty() && parts->empty()) || parts->size() > options.max_reply_bytes)
      co_return std::unexpected(Error::invalid_argument("invalid channel reply parts"));
    for (const auto& part : *parts) {
      auto valid = split_text(part, limit);
      if (part.empty() || !valid || valid->size() != 1)
        co_return std::unexpected(Error::invalid_argument("invalid channel reply part"));
    }
    entry.reply.parts = std::move(*parts);
    entry.generated = true;
    if (cancelled)
      co_return std::unexpected(Error::cancelled());
    co_return co_await deliver(entry);
  }
};

Dispatcher::Dispatcher(std::unique_ptr<Impl> impl, PrivateTag) : impl_{std::move(impl)} {}
Dispatcher::~Dispatcher() = default;
Result<std::unique_ptr<Dispatcher>>
Dispatcher::create(const Adapter& adapter, Transport transport, RunTurn run, DispatcherOptions options) {
  if (!transport || !run || !options.hooks || !options.capacity || options.capacity > 4096 ||
      !options.max_reply_bytes || options.max_reply_bytes > 1024 * 1024 || options.typing_interval.count() <= 0 ||
      options.typing_interval > std::chrono::milliseconds{4000} || options.typing_ttl.count() <= 0 ||
      options.typing_ttl > std::chrono::seconds{60})
    return std::unexpected(Error::invalid_argument("invalid channel dispatcher options"));
  auto impl = std::make_unique<Impl>(adapter, std::move(transport), std::move(run), std::move(options));
  // Private constructor prevents construction without validated ports.
  return std::make_unique<Dispatcher>(std::move(impl), PrivateTag{});
}

async::Awaitable<Result<Delivery>> Dispatcher::handle_impl(Message message) {
  auto valid = detail::validate(message, impl_->adapter.platform());
  if (!valid)
    co_return std::unexpected(valid.error());
  const auto conversation = conversation_key(message.conversation);
  const auto key = event_key(message);
  if (impl_->busy.contains(conversation))
    co_return std::unexpected(Error{core::ErrorKind::conflict, "channel conversation is busy"});
  if (impl_->busy.size() >= impl_->options.capacity)
    co_return std::unexpected(Error{core::ErrorKind::mailbox_overflowed, "channel admission is full"});
  // Admission and duplicate lookup must not interleave while permission hooks await.
  ConversationLease lease{impl_->busy, conversation};
  auto allowed = co_await impl_->authorize(message, "ChannelReceive", nullptr);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  if (auto found = impl_->entries.find(key); found != impl_->entries.end()) {
    if (found->second.error)
      co_return std::unexpected(*found->second.error);
    auto result = found->second.delivery;
    result.duplicate = true;
    co_return result;
  }
  if (impl_->entries.size() >= impl_->options.capacity) {
    if (impl_->completed.empty())
      co_return std::unexpected(Error{core::ErrorKind::mailbox_overflowed, "channel delivery cache is full"});
    impl_->entries.erase(impl_->completed.front());
    impl_->completed.pop_front();
  }
  auto [it, inserted] = impl_->entries.emplace(key, Entry{.message = std::move(message)});
  static_cast<void>(inserted);
  auto& entry = it->second;
  Result<Delivery> result = std::unexpected(Error::internal("channel dispatch failed"));
  try {
    result = co_await impl_->process(entry);
  } catch (...) { /* Preserve a terminal error, never replay the turn. */
  }
  if (!result)
    entry.error = result.error();
  co_return result;
}

Result<PendingReply> Dispatcher::pending(const Message& message) const {
  const auto key = event_key(message);
  if (impl_->busy.contains(conversation_key(message.conversation)))
    return std::unexpected(Error{core::ErrorKind::conflict, "channel conversation is busy"});
  auto found = impl_->entries.find(key);
  if (found == impl_->entries.end() || !found->second.generated || found->second.complete)
    return std::unexpected(Error::not_found("no pending channel reply"));
  return found->second.reply;
}

async::Awaitable<Result<Delivery>> Dispatcher::resume_impl(Message message, std::size_t next_part) {
  const auto conversation = conversation_key(message.conversation);
  if (impl_->busy.contains(conversation))
    co_return std::unexpected(Error{core::ErrorKind::conflict, "channel conversation is busy"});
  auto found = impl_->entries.find(event_key(message));
  if (found == impl_->entries.end() || !found->second.generated || found->second.complete)
    co_return std::unexpected(Error::not_found("no pending channel reply"));
  auto& entry = found->second;
  if (next_part < entry.reply.next_part || next_part > entry.reply.parts.size())
    co_return std::unexpected(Error::invalid_argument("invalid delivery recovery position"));
  ConversationLease lease{impl_->busy, conversation};
  auto allowed = co_await impl_->authorize(entry.message, "ChannelReceive", nullptr);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  entry.reply.next_part = next_part;
  entry.delivery.parts_sent = next_part;
  auto result = co_await impl_->deliver(entry);
  if (!result)
    entry.error = result.error();
  co_return result;
}

async::Awaitable<Result<Delivery>> Dispatcher::handle(Message message) {
  try {
    co_return co_await handle_impl(std::move(message));
  } catch (...) { /* Translate callback and cancellation exceptions at the boundary. */
  }
  const auto state = co_await asio::this_coro::cancellation_state;
  co_return std::unexpected(state.cancelled() != asio::cancellation_type::none
                                ? Error::cancelled()
                                : Error::internal("channel dispatch failed"));
}

async::Awaitable<Result<Delivery>> Dispatcher::resume(Message message, std::size_t next_part) {
  try {
    co_return co_await resume_impl(std::move(message), next_part);
  } catch (...) { /* Recovery retains the pending reply on exceptions. */
  }
  const auto state = co_await asio::this_coro::cancellation_state;
  co_return std::unexpected(state.cancelled() != asio::cancellation_type::none
                                ? Error::cancelled()
                                : Error::internal("channel recovery failed"));
}

Result<void> Dispatcher::acknowledge_failure(const Message& message) {
  if (impl_->busy.contains(conversation_key(message.conversation)))
    return std::unexpected(Error{core::ErrorKind::conflict, "channel conversation is busy"});
  auto found = impl_->entries.find(event_key(message));
  if (found == impl_->entries.end() || !found->second.error)
    return std::unexpected(Error::not_found("no failed channel delivery"));
  if (!found->second.complete) {
    found->second.complete = true;
    impl_->completed.push_back(found->first);
  }
  return {};
}
}  // namespace orangutan::channel
