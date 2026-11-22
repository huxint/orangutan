#include "host.hpp"

#include <limits>

namespace orangutan::telegram_host {
namespace {
using core::Error;
using core::Result;

Result<State> decode_state(std::string_view bytes) {
  try {
    const auto json = Json::parse(bytes);
    auto session = core::parse_turn_id_hex(json.at("session").get<std::string>());
    if (!session || core::is_zero_turn_id(*session) || !json.at("next_update").is_number_integer())
      return std::unexpected(Error::parsing("invalid Telegram journal identity or cursor"));
    State state{.bot = json.at("bot").get<std::string>(),
                .user = json.at("user").get<std::string>(),
                .workspace = json.at("workspace").get<std::string>(),
                .session = *session,
                .next_update = json.at("next_update").get<std::int64_t>()};
    if (state.next_update < 0)
      return std::unexpected(Error::parsing("invalid Telegram journal cursor"));
    const auto& pending = json.at("pending");
    if (!pending.is_null()) {
      const auto& id = pending.at("update").at("update_id");
      const auto& count = pending.at("confirmed_parts");
      if (!id.is_number_integer() || id.get<std::int64_t>() < state.next_update ||
          id.get<std::int64_t>() == std::numeric_limits<std::int64_t>::max() || !count.is_number_integer() ||
          count.get<std::int64_t>() < 0)
        return std::unexpected(Error::parsing("invalid Telegram pending delivery"));
      state.pending =
          Pending{.update = pending.at("update"),
                  .answer = pending.at("answer").is_null() ? std::nullopt
                                                           : std::optional{pending.at("answer").get<std::string>()},
                  .confirmed_parts = count.get<std::size_t>(),
                  .send_inflight = pending.at("send_inflight").get<bool>()};
      state.pending->task_id = pending.value("task_id", std::string{});
      if (!state.pending->task_id.empty()) {
        auto task = core::parse_turn_id_hex(state.pending->task_id);
        if (!task || core::is_zero_turn_id(*task))
          return std::unexpected(Error::parsing("invalid pending background task identifier"));
      }
    }
    return state;
  } catch (const Json::exception&) {
    return std::unexpected(Error::parsing("invalid Telegram state journal"));
  }
}
}  // namespace

Json encode_state(const State& state) {
  Json pending = nullptr;
  if (state.pending) {
    const auto& entry = *state.pending;
    pending = Json{{"update", entry.update},
                   {"answer", entry.answer ? Json(*entry.answer) : Json(nullptr)},
                   {"confirmed_parts", entry.confirmed_parts},
                   {"send_inflight", entry.send_inflight}};
    if (!entry.task_id.empty())
      pending["task_id"] = entry.task_id;
  }
  return Json{{"bot", state.bot},
              {"user", state.user},
              {"workspace", state.workspace},
              {"session", core::format_turn_id_hex(state.session)},
              {"next_update", state.next_update},
              {"pending", std::move(pending)}};
}

Result<State> load_state(const io::PrivateDirectory& directory, std::string_view user, std::string_view workspace) {
  auto bytes = directory.read("state.json", 2 * 1024 * 1024);
  if (!bytes)
    return std::unexpected(bytes.error());
  if (!*bytes) {
    auto id = core::generate_turn_id();
    if (!id)
      return std::unexpected(id.error());
    return State{.bot = {}, .user = std::string{user}, .workspace = std::string{workspace}, .session = *id};
  }
  auto state = decode_state(**bytes);
  if (!state)
    return std::unexpected(state.error());
  if (state->user != user || state->workspace != workspace || state->pending)
    return std::unexpected(
        Error{core::ErrorKind::conflict, "state binding mismatch or unresolved pending delivery; inspect state.json"});
  return state;
}

Result<void> acknowledge_pending(const io::PrivateDirectory& directory, std::int64_t update) {
  auto bytes = directory.read("state.json", 2 * 1024 * 1024);
  if (!bytes)
    return std::unexpected(bytes.error());
  if (!*bytes)
    return std::unexpected(Error::not_found("no Telegram journal"));
  auto state = decode_state(**bytes);
  if (!state)
    return std::unexpected(state.error());
  if (!state->pending || state->pending->update.at("update_id") != update)
    return std::unexpected(Error{core::ErrorKind::conflict, "pending update differs from acknowledgment"});
  const auto task_id = state->pending->task_id;
  auto archived = directory.write(task_id.empty() ? "handled-" + std::to_string(update) + ".json"
                                                  : "handled-task-" + task_id + ".json",
                                  **bytes);
  if (!archived)
    return std::unexpected(archived.error());
  if (task_id.empty())
    state->next_update = update + 1;
  state->pending.reset();
  return directory.write("state.json", encode_state(*state).dump(2) + "\n");
}
}  // namespace orangutan::telegram_host
