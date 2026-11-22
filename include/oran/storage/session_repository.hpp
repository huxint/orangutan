#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/role.hpp>
#include <oran/storage/migrations.hpp>

namespace orangutan::storage {

class Pool;

struct SessionKey {
  std::string session_id;
  std::string agent_key;
};

struct AppendSessionMessageRequest {
  std::string session_id;
  std::string agent_key;
  core::Role role{core::Role::user};
  std::string content_json;
  std::string metadata_json{"{}"};
};

struct SessionMessageInput {
  core::Role role{core::Role::user};
  std::string content_json;
  std::string metadata_json{"{}"};
};

struct SessionMessageRecord {
  std::string session_id;
  std::string agent_key;
  std::int64_t sequence{};
  core::Role role{core::Role::user};
  std::string content_json;
  std::string metadata_json;
  std::string created_at;
};

struct SessionRecord {
  std::string session_id;
  std::string agent_key;
  std::optional<std::string> title;
  std::string metadata_json;
  std::string created_at;
  std::string updated_at;
  std::int64_t message_count{};
};

struct SessionCommit {
  std::int64_t expected_messages{};
  std::int64_t expected_revision{};
  /// Validated working-context JSON, stored under oran_working_context.
  std::string checkpoint_json;
};

struct SessionRepositoryOptions {
  std::string migrations_directory;
};

class SessionRepository {
public:
  explicit SessionRepository(Pool& pool, SessionRepositoryOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MigrationReport>> migrate();

  [[nodiscard]] async::Awaitable<core::Result<SessionMessageRecord>>
  append_message(AppendSessionMessageRequest request);

  /// Appends an ordered suffix and optional checkpoint in one transaction.
  /// An empty suffix without a checkpoint has no effect.
  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionMessageRecord>>>
  append_messages(SessionKey key,
                  std::vector<SessionMessageInput> messages,
                  std::optional<SessionCommit> commit = std::nullopt);

  /// Forward page; refuses an oversized first row rather than skipping it.
  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionMessageRecord>>>
  load_after(SessionKey key,
             std::int64_t after,
             std::int64_t through,
             std::size_t max_messages = 64,
             std::size_t max_bytes = 512 * 1024);

  /// Explicit host operation; source is a read-only session database snapshot.
  /// Refuses any existing destination identity. Preserves messages and metadata.
  [[nodiscard]] async::Awaitable<core::Result<void>>
  import_session(std::string source_path, SessionKey source, SessionKey destination);

  [[nodiscard]] async::Awaitable<core::Result<void>> backup_to(std::string destination);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionMessageRecord>>> load_messages(SessionKey key);
  /// Latest rows in conversation order, bounded by encoded bytes and count.
  /// Stops before a row that would exceed the byte budget; stored history is unchanged.
  [[nodiscard]] async::Awaitable<core::Result<std::vector<SessionMessageRecord>>>
  load_tail(SessionKey key, std::size_t max_messages = 128, std::size_t max_bytes = 512 * 1024);

  [[nodiscard]] async::Awaitable<core::Result<std::optional<SessionRecord>>> get_session(SessionKey key);

private:
  Pool* pool_{};
  SessionRepositoryOptions options_;
};

}  // namespace orangutan::storage
