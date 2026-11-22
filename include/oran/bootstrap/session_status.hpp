#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <asio/any_io_executor.hpp>
#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/turn_id.hpp>

namespace orangutan::bootstrap {
class RuntimeAssembly;

struct SessionUsage {
  std::string model;
  std::int64_t input_tokens{};
  std::int64_t output_tokens{};
  std::int64_t cache_read_tokens{};
};
struct SessionStatus {
  std::optional<std::int64_t> saved_messages;
  std::int64_t summarized_messages{};
  bool longterm_memory_enabled{};
  bool trace_enabled{};
  std::optional<SessionUsage> last_turn;
};

/// Caller authorizes inspection. Reads bounded metadata on worker; no writes or transcript content.
[[nodiscard]] async::Awaitable<core::Result<SessionStatus>>
inspect_session(RuntimeAssembly& assembly, core::TurnId session, std::string agent_key, asio::any_io_executor worker);
}  // namespace orangutan::bootstrap
