#pragma once

#include <asio/steady_timer.hpp>
#include <oran/async/task_group.hpp>
#include <oran/channel/dispatcher.hpp>
#include <oran/hook/event.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::telegram_host {
/// One strand owns streaming buffers and status. deliver() joins the updater
/// before returning; provider callbacks only mutate values, never start IO.
class Presentation final : public provider::EventSink {
public:
  Presentation(asio::any_io_executor executor,
               channel::Transport transport,
               hook::Bus& hooks,
               permission::RuleSet rules,
               std::chrono::milliseconds interval = std::chrono::seconds{1});
  [[nodiscard]] async::Awaitable<core::Result<channel::Delivery>> deliver(channel::Dispatcher& dispatcher,
                                                                          channel::Message message);
  void observe(hook::Event event);
  /// Called only from the dispatcher-admitted turn callback.
  void accepted();
  /// The admitted turn drains previews before returning its final answer.
  [[nodiscard]] async::Awaitable<core::Result<void>> prepare_final();
  void on_text_delta(std::string_view delta) override;
  void on_thinking_delta(std::string_view delta) override;
  void on_tool_start(std::string_view id, std::string_view name) override;

private:
  enum class Phase {
    received,
    thinking,
    tool,
    answering,
    done,
    failed
  };
  [[nodiscard]] async::Awaitable<core::Result<void>> update();
  [[nodiscard]] async::Awaitable<bool> send(std::string operation, channel::Request request);
  [[nodiscard]] async::Awaitable<void> react(Phase phase);
  asio::steady_timer timer_;
  std::optional<async::TaskGroup> tasks_;
  channel::Transport transport_;
  hook::Bus& hooks_;
  permission::RuleSet rules_;
  std::chrono::milliseconds interval_;
  std::optional<channel::Message> message_;
  std::int64_t chat_id_{0};
  std::int64_t message_id_{0};
  Phase phase_{Phase::received};
  std::optional<Phase> shown_phase_;
  std::string text_;
  std::string shown_text_;
  unsigned reaction_failures_{0};
  unsigned draft_failures_{0};
  bool closing_{false};
  bool admitted_{false};
  std::chrono::steady_clock::time_point retry_at_{};
};
}  // namespace orangutan::telegram_host
