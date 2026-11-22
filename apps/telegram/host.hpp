#pragma once

#include <asio/any_io_executor.hpp>
#include <functional>
#include <nlohmann/json.hpp>
#include <oran/channel/dispatcher.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/io/private_directory.hpp>

namespace orangutan::telegram_host {
class Presentation;
using Json = nlohmann::json;
enum class PollMethod {
  getMe,
  getWebhookInfo,
  getUpdates,
  setMyCommands
};
using Api = std::function<async::Awaitable<core::Result<channel::Response>>(PollMethod, std::string)>;
using StatusReader = std::function<async::Awaitable<core::Result<std::string>>(core::TurnId)>;
enum class TaskCommand {
  list,
  stop
};
struct BackgroundPort {
  std::function<bool(core::TurnId)> active;
  std::function<std::optional<std::string>(core::TurnId)> next;
  std::function<async::Awaitable<core::Result<std::string>>(core::TurnId, std::string)> complete;
  std::function<core::Result<std::string>(core::TurnId, TaskCommand)> control;
};

struct Options {
  std::string user{};
  bool probe{false};
  bool once{false};
};

struct Pending {
  Json update;
  std::optional<std::string> answer;
  std::size_t confirmed_parts{0};
  bool send_inflight{false};
  std::string task_id{};
};

struct State {
  std::string bot;
  std::string user;
  std::string workspace;
  core::TurnId session{};
  std::int64_t next_update{0};
  std::optional<Pending> pending{};
};

/// A pending journal is never replayed automatically, including after a crash.
[[nodiscard]] core::Result<State>
load_state(const io::PrivateDirectory& directory, std::string_view user, std::string_view workspace);
[[nodiscard]] Json encode_state(const State& state);
/// Explicit operator reconciliation: archive pending data, then skip exactly this update.
[[nodiscard]] core::Result<void> acknowledge_pending(const io::PrivateDirectory& directory, std::int64_t update);
[[nodiscard]] bool admits(const channel::Message& message, std::string_view user);
[[nodiscard]] core::Result<Json> api_result(const channel::Response& response);

/// Injected effects are called only after explicit permission/hook decisions.
/// All calls run on one strand; the caller retains services until completion.
[[nodiscard]] async::Awaitable<core::Result<void>> run(Options options,
                                                       Api api,
                                                       channel::Transport outbound,
                                                       channel::RunTurn turn,
                                                       hook::Bus& hooks,
                                                       io::PrivateDirectory& directory,
                                                       State& state,
                                                       asio::any_io_executor worker,
                                                       Presentation* presentation = nullptr,
                                                       StatusReader status = {},
                                                       BackgroundPort background = {});
}  // namespace orangutan::telegram_host
