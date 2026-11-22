#pragma once

#include <cstddef>
#include <optional>
#include <oran/core/working_context.hpp>
#include <span>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/message.hpp>
#include <oran/core/result.hpp>

namespace orangutan::storage {
class SessionRepository;
}  // namespace orangutan::storage

namespace orangutan::memory::session {

struct SessionId {
  std::string value;

  friend bool operator==(const SessionId&, const SessionId&) = default;
};

struct AgentKey {
  std::string value;

  friend bool operator==(const AgentKey&, const AgentKey&) = default;
};

struct ContextSnapshot {
  core::WorkingContext checkpoint;
  std::int64_t message_count{};
};

class Store {
public:
  explicit Store(storage::SessionRepository& repository) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<void>>
  append(SessionId session_id, AgentKey agent_key, core::Message message);

  /// Serializes the whole suffix before writing it atomically. The caller retains
  /// the messages until completion. Without a checkpoint, an empty suffix has
  /// no effect; an explicit snapshot also atomically updates checkpoint metadata.
  [[nodiscard]] async::Awaitable<core::Result<void>> append_all(SessionId session_id,
                                                                AgentKey agent_key,
                                                                std::span<const core::Message> messages,
                                                                std::optional<ContextSnapshot> expected = std::nullopt,
                                                                core::WorkingContext checkpoint = {});

  [[nodiscard]] async::Awaitable<core::Result<ContextSnapshot>> load_context(SessionId session_id, AgentKey agent_key);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<core::Message>>> load_after(SessionId session_id,
                                                                                      AgentKey agent_key,
                                                                                      std::int64_t after,
                                                                                      std::int64_t through,
                                                                                      std::size_t max_bytes = 512 *
                                                                                                              1024);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<core::Message>>> load(SessionId session_id,
                                                                                AgentKey agent_key);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<core::Message>>> load_tail(SessionId session_id,
                                                                                     AgentKey agent_key,
                                                                                     std::size_t max_messages = 128,
                                                                                     std::size_t max_bytes = 512 *
                                                                                                             1024);

private:
  storage::SessionRepository* repository_{};
};

}  // namespace orangutan::memory::session
