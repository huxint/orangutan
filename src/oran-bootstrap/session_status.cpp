#include <oran/bootstrap/session_status.hpp>

#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/memory/session.hpp>
#include <oran/storage/trace_repository.hpp>

namespace orangutan::bootstrap {
async::Awaitable<core::Result<SessionStatus>>
inspect_session(RuntimeAssembly& assembly, core::TurnId session, std::string agent_key, asio::any_io_executor worker) {
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(core::Error::cancelled());
  if (agent_key.empty() || core::is_zero_turn_id(session) || !worker)
    co_return std::unexpected(core::Error::invalid_argument("invalid session inspection identity or executor"));
  SessionStatus status{.saved_messages = {},
                       .summarized_messages = 0,
                       .longterm_memory_enabled = assembly.longterm_memory_enabled(),
                       .trace_enabled = assembly.trace_enabled(),
                       .last_turn = {}};
  if (auto* store = assembly.session_store()) {
    auto snapshot = co_await asio::co_spawn(worker,
                                            store->load_context({core::format_turn_id_hex(session)}, {agent_key}),
                                            asio::use_awaitable);
    if (!snapshot)
      co_return std::unexpected(snapshot.error());
    status.saved_messages = snapshot->message_count;
    status.summarized_messages = snapshot->checkpoint.covered_sequence;
  }
  if (auto* trace = assembly.trace_repository()) {
    auto turns = co_await asio::co_spawn(
        worker,
        trace->list_turns({.session_id = session, .agent_key = std::move(agent_key), .limit = 1}),
        asio::use_awaitable);
    if (!turns)
      co_return std::unexpected(turns.error());
    if (!turns->empty()) {
      const auto& last = turns->front();
      status.last_turn = SessionUsage{last.route_model, last.input_tokens, last.output_tokens, last.cache_read_tokens};
    }
  }
  co_return status;
}
}  // namespace orangutan::bootstrap
