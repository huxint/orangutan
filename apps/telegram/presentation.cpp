#include "presentation.hpp"
#include "format.hpp"
#include <charconv>

#include <asio/redirect_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <oran/async/task_group.hpp>
#include <oran/core/str.hpp>
#include <oran/hook/bus.hpp>

namespace orangutan::telegram_host {
using core::Error;
using core::Result;
using Json = nlohmann::json;

Presentation::Presentation(asio::any_io_executor executor,
                           channel::Transport transport,
                           hook::Bus& hooks,
                           permission::RuleSet rules,
                           std::chrono::milliseconds interval)
    : timer_{std::move(executor)}, transport_{std::move(transport)}, hooks_{hooks}, rules_{std::move(rules)},
      interval_{interval} {}

void Presentation::observe(hook::Event event) {
  if (!message_)
    return;
  if (event == hook::Event::provider_request) {
    phase_ = Phase::thinking;
    text_.clear();  // Retries and tool continuations start a new provider answer.
  } else if (event == hook::Event::tool_before) {
    phase_ = Phase::tool;
  } else if (event == hook::Event::tool_after) {
    phase_ = Phase::thinking;
  }
}
void Presentation::accepted() {
  admitted_ = true;
  timer_.cancel();
}
async::Awaitable<Result<void>> Presentation::prepare_final() {
  closing_ = true;
  timer_.cancel();
  auto joined = co_await tasks_->join();
  if (!joined)
    co_return std::unexpected(joined.error());
  co_return Result<void>{};
}
void Presentation::on_text_delta(std::string_view delta) {
  if (!message_)
    return;
  phase_ = Phase::answering;
  text_ += core::str::truncate_to_code_point(delta, 65536 - text_.size());
}
void Presentation::on_thinking_delta(std::string_view) {
  if (message_)
    phase_ = Phase::thinking;
}
void Presentation::on_tool_start(std::string_view, std::string_view) {
  if (message_)
    phase_ = Phase::tool;
}

async::Awaitable<bool> Presentation::send(std::string operation, channel::Request request) {
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none || std::chrono::steady_clock::now() < retry_at_)
    co_return false;
  const std::array required{core::Capability::egress_http};
  const auto input = Json{
      {"conversation", channel::conversation_key(message_->conversation)},
      {"sender", message_->sender},
      {"path", request.path},
      {"body",
       request.body}}.dump();
  const auto decision = permission::evaluate(rules_, operation, input, required, permission::Mode::strict);
  co_await hooks_.publish_advisory(hook::Event::channel_action,
                                   hook::ChannelActionPayload{"telegram",
                                                              message_->conversation.account,
                                                              operation,
                                                              decision.verdict == permission::Verdict::allow});
  if (decision.verdict != permission::Verdict::allow)
    co_return false;
  try {
    auto response = co_await transport_(message_->conversation, std::move(request));
    if (!response)
      co_return false;
    auto accepted = channel::telegram().accept(*response);
    if (!accepted && accepted.error().kind() == core::ErrorKind::rate_limit)
      retry_at_ = std::chrono::steady_clock::now() + accepted.error().retry_after().value_or(std::chrono::seconds{30});
    co_return accepted.has_value();
  } catch (...) {
    // Presentation is advisory; credential-bearing transport errors stay private.
    co_return false;
  }
}

async::Awaitable<void> Presentation::react(Phase phase) {
  if (reaction_failures_ >= 2 || shown_phase_ == phase)
    co_return;
  std::string_view emoji;
  switch (phase) {
    case Phase::received:
      emoji = "👀";
      break;
    case Phase::thinking:
      emoji = "🤔";
      break;
    case Phase::tool:
      emoji = "👨‍💻";
      break;
    case Phase::answering:
      emoji = "✍";
      break;
    case Phase::done:
      emoji = "👍";
      break;
    case Phase::failed:
      emoji = "😱";
      break;
  }
  const Json body{{"chat_id", message_->conversation.chat},
                  {"message_id", message_id_},
                  {"reaction", Json::array({Json{{"type", "emoji"}, {"emoji", emoji}}})}};
  auto sent = co_await send("ChannelStatus", {"POST", "/setMessageReaction", body.dump()});
  shown_phase_ = phase;
  if (!sent)
    ++reaction_failures_;
}

async::Awaitable<Result<void>> Presentation::update() {
  while (!admitted_ && !closing_) {
    timer_.expires_at(std::chrono::steady_clock::time_point::max());
    asio::error_code error;
    co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, error));
    if ((co_await asio::this_coro::cancellation_state).cancelled() != asio::cancellation_type::none)
      co_return std::unexpected(Error::cancelled());
  }
  if (closing_)
    co_return Result<void>{};
  co_await react(Phase::received);
  auto draft_at = std::chrono::steady_clock::now();
  while (!closing_) {
    timer_.expires_after(interval_);
    asio::error_code error;
    co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, error));
    if (closing_)
      break;
    if (error)
      co_return std::unexpected(Error::cancelled());
    co_await react(phase_);
    const auto now = std::chrono::steady_clock::now();
    if (draft_failures_ >= 2 || text_.empty() || (text_ == shown_text_ && now < draft_at))
      continue;
    // Snapshot before awaiting IO; new deltas remain pending for the next tick.
    shown_text_ = text_;
    auto formatted = format_markdown(shown_text_);
    if (!formatted)
      continue;
    auto parts = split_formatted(*formatted);
    if (!parts || parts->empty())
      continue;
    const auto& preview = parts->back();
    const Json body{{"chat_id", chat_id_},
                    {"draft_id", message_id_},
                    {"text", preview.text},
                    {"entities", preview.entities}};
    auto sent = co_await send("ChannelDraft", {"POST", "/sendMessageDraft", body.dump()});
    if (!sent)
      ++draft_failures_;
    draft_at = now + std::chrono::seconds{15};
  }
  co_return Result<void>{};
}

async::Awaitable<Result<channel::Delivery>> Presentation::deliver(channel::Dispatcher& dispatcher,
                                                                  channel::Message message) {
  const auto number = [](std::string_view text, std::int64_t& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value > 0;
  };
  if (!number(message.conversation.chat, chat_id_) || !number(message.message_id, message_id_))
    co_return std::unexpected(Error::invalid_argument("invalid Telegram private message identifier"));
  message_ = message;
  phase_ = Phase::received;
  shown_phase_.reset();
  text_.clear();
  shown_text_.clear();
  reaction_failures_ = draft_failures_ = 0;
  closing_ = false;
  admitted_ = false;
  auto tasks = async::TaskGroup::create(timer_.get_executor(), {.max_tasks = 1, .max_completed = 1});
  if (!tasks)
    co_return std::unexpected(tasks.error());
  tasks_.emplace(std::move(*tasks));
  auto spawned = tasks_->spawn("telegram-presentation", [this] { return update(); });
  if (!spawned)
    co_return std::unexpected(spawned.error());
  std::optional<Result<channel::Delivery>> delivered;
  std::exception_ptr exception;
  try {
    delivered = co_await dispatcher.handle(std::move(message));
  } catch (...) {
    exception = std::current_exception();
  }
  bool cancelled = (co_await asio::this_coro::cancellation_state).cancelled() != asio::cancellation_type::none;
  // Finish in-flight advisory IO before releasing its borrowed turn state.
  co_await asio::this_coro::reset_cancellation_state([&cancelled](asio::cancellation_type_t type) {
    cancelled |= type != asio::cancellation_type::none;
    return asio::cancellation_type::none;
  });
  co_await asio::this_coro::throw_if_cancelled(false);
  closing_ = true;
  timer_.cancel();
  auto joined = co_await tasks_->join();
  tasks_.reset();
  if (admitted_)
    co_await react(delivered && *delivered ? Phase::done : Phase::failed);
  message_.reset();
  co_await asio::this_coro::reset_cancellation_state(asio::enable_terminal_cancellation());
  if (exception)
    co_return std::unexpected(cancelled ? Error::cancelled() : Error::internal("Telegram delivery failed"));
  if (!*delivered)
    co_return std::unexpected(delivered->error());
  if (cancelled)
    co_return std::unexpected(Error::cancelled());
  (*delivered)->activity_failures += reaction_failures_ + draft_failures_ + (joined ? joined->failed() : 1);
  co_return std::move(**delivered);
}
}  // namespace orangutan::telegram_host
