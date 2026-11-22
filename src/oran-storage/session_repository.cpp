#include <oran/storage/session_repository.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/core/role.hpp>
#include <oran/core/str.hpp>
#include <oran/storage/migrations.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::storage {

namespace {

constexpr std::string_view kAppendMessageSql = R"sql(
INSERT INTO session_messages(session_id, agent_key, sequence, role, content_json, metadata_json, created_at)
VALUES (
  ?, ?,
  (
    SELECT COALESCE(MAX(sequence), 0) + 1
    FROM session_messages
    WHERE session_id = ? AND agent_key = ?
  ),
  ?, ?, ?,
  strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
)
RETURNING sequence, created_at
)sql";

constexpr std::string_view kLoadMessagesSql = R"sql(
SELECT session_id, agent_key, sequence, role, content_json, metadata_json, created_at
FROM session_messages
WHERE session_id = ? AND agent_key = ?
ORDER BY sequence ASC
)sql";

constexpr std::string_view LOAD_TAIL_SQL = R"sql(
SELECT session_id, agent_key, sequence, role, content_json, metadata_json, created_at,
       length(CAST(content_json AS BLOB)) + length(CAST(metadata_json AS BLOB))
FROM session_messages
WHERE session_id = ? AND agent_key = ?
ORDER BY sequence DESC LIMIT ?
)sql";

constexpr std::string_view kGetSessionSql = R"sql(
SELECT s.session_id,
       s.agent_key,
       s.title,
       s.metadata_json,
       s.created_at,
       s.updated_at,
       COUNT(m.sequence) AS message_count
FROM sessions AS s
LEFT JOIN session_messages AS m
  ON m.session_id = s.session_id AND m.agent_key = s.agent_key
WHERE s.session_id = ? AND s.agent_key = ?
GROUP BY s.session_id, s.agent_key, s.title, s.metadata_json, s.created_at, s.updated_at
)sql";

[[nodiscard]] core::Error invalid_field(std::string field) {
  return core::Error::invalid_argument("session repository field must not be empty").with("field", std::move(field));
}

[[nodiscard]] core::Result<void> validate_key(const SessionKey& key) {
  if (key.session_id.empty()) {
    return std::unexpected(invalid_field("session_id"));
  }
  if (key.agent_key.empty()) {
    return std::unexpected(invalid_field("agent_key"));
  }
  return {};
}

[[nodiscard]] core::Result<SessionMessageRecord> read_message_row(Statement& statement) {
  auto session_id = statement.required_text(0, "session_id");
  if (!session_id) {
    return std::unexpected(session_id.error());
  }
  auto agent_key = statement.required_text(1, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  auto sequence = statement.column_int64(2);
  if (!sequence) {
    return std::unexpected(sequence.error().with("field", "sequence"));
  }
  auto role_text = statement.required_text(3, "role");
  if (!role_text) {
    return std::unexpected(role_text.error());
  }
  auto role = core::parse_enum<core::Role>(*role_text);
  if (!role) {
    return std::unexpected(
        core::Error::storage("session repository row has unknown role").with("role", std::move(*role_text)));
  }
  auto content_json = statement.required_text(4, "content_json");
  if (!content_json) {
    return std::unexpected(content_json.error());
  }
  auto metadata_json = statement.required_text(5, "metadata_json");
  if (!metadata_json) {
    return std::unexpected(metadata_json.error());
  }
  auto created_at = statement.required_text(6, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }

  return SessionMessageRecord{
      .session_id = std::move(*session_id),
      .agent_key = std::move(*agent_key),
      .sequence = *sequence,
      .role = *role,
      .content_json = std::move(*content_json),
      .metadata_json = std::move(*metadata_json),
      .created_at = std::move(*created_at),
  };
}

[[nodiscard]] core::Result<SessionRecord> read_session_row(Statement& statement) {
  auto session_id = statement.required_text(0, "session_id");
  if (!session_id) {
    return std::unexpected(session_id.error());
  }
  auto agent_key = statement.required_text(1, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  auto title = statement.column_text(2);
  if (!title) {
    return std::unexpected(title.error());
  }
  auto metadata_json = statement.required_text(3, "metadata_json");
  if (!metadata_json) {
    return std::unexpected(metadata_json.error());
  }
  auto created_at = statement.required_text(4, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  auto updated_at = statement.required_text(5, "updated_at");
  if (!updated_at) {
    return std::unexpected(updated_at.error());
  }
  auto message_count = statement.column_int64(6);
  if (!message_count) {
    return std::unexpected(message_count.error().with("field", "message_count"));
  }

  return SessionRecord{
      .session_id = std::move(*session_id),
      .agent_key = std::move(*agent_key),
      .title = std::move(*title),
      .metadata_json = std::move(*metadata_json),
      .created_at = std::move(*created_at),
      .updated_at = std::move(*updated_at),
      .message_count = *message_count,
  };
}

[[nodiscard]] core::Result<SessionMessageRecord>
insert_message(Statement& statement, const SessionKey& key, SessionMessageInput message) {
  if (auto bound = statement.bind_all(key.session_id,
                                      key.agent_key,
                                      key.session_id,
                                      key.agent_key,
                                      core::enum_name(message.role),
                                      message.content_json,
                                      message.metadata_json);
      !bound) {
    return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    return std::unexpected(step.error());
  }
  if (*step != StepResult::row) {
    return std::unexpected(core::Error::storage("session message insert returned no row"));
  }

  auto sequence = statement.column_int64(0);
  if (!sequence) {
    return std::unexpected(sequence.error().with("field", "sequence"));
  }
  auto created_at = statement.required_text(1, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  if (auto done = statement.expect_done("append_message"); !done) {
    return std::unexpected(done.error());
  }

  return SessionMessageRecord{
      .session_id = key.session_id,
      .agent_key = key.agent_key,
      .sequence = *sequence,
      .role = message.role,
      .content_json = std::move(message.content_json),
      .metadata_json = std::move(message.metadata_json),
      .created_at = std::move(*created_at),
  };
}

}  // namespace

SessionRepository::SessionRepository(Pool& pool, SessionRepositoryOptions options) noexcept
    : pool_{&pool}, options_{std::move(options)} {}

async::Awaitable<core::Result<MigrationReport>> SessionRepository::migrate() {
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  if (options_.migrations_directory.empty()) {
    auto report = run_migrations(writer->connection(), built_in_session_migrations());
    if (!report) {
      co_return std::unexpected(report.error());
    }
    co_return std::move(*report);
  }

  auto report = run_migrations_from_directory(writer->connection(), options_.migrations_directory);
  if (!report) {
    co_return std::unexpected(report.error());
  }
  co_return std::move(*report);
}

async::Awaitable<core::Result<SessionMessageRecord>>
SessionRepository::append_message(AppendSessionMessageRequest request) {
  auto messages = std::vector<SessionMessageInput>{};
  messages.push_back(SessionMessageInput{.role = request.role,
                                         .content_json = std::move(request.content_json),
                                         .metadata_json = std::move(request.metadata_json)});
  auto appended = co_await append_messages(
      SessionKey{.session_id = std::move(request.session_id), .agent_key = std::move(request.agent_key)},
      std::move(messages));
  if (!appended) {
    co_return std::unexpected(std::move(appended).error());
  }
  co_return std::move(appended->front());
}

async::Awaitable<core::Result<std::vector<SessionMessageRecord>>>
SessionRepository::append_messages(SessionKey key,
                                   std::vector<SessionMessageInput> messages,
                                   std::optional<SessionCommit> commit) {
  if (auto valid = validate_key(key); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  for (std::size_t i = 0; i < messages.size(); ++i) {
    if (messages[i].content_json.empty()) {
      co_return std::unexpected(invalid_field("content_json").with("index", std::to_string(i)));
    }
    if (messages[i].metadata_json.empty()) {
      co_return std::unexpected(invalid_field("metadata_json").with("index", std::to_string(i)));
    }
  }

  if (commit && (commit->expected_messages < 0 || commit->expected_revision < 0 ||
                 commit->checkpoint_json.size() > 512 * 1024 || !core::str::is_valid_utf8(commit->checkpoint_json)))
    co_return std::unexpected(core::Error::invalid_argument("invalid checkpoint commit"));
  auto records = std::vector<SessionMessageRecord>{};
  if (messages.empty() && !commit) {
    co_return records;
  }
  records.reserve(messages.size());
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }

  auto& connection = writer->connection();
  auto transaction = Transaction::begin(connection);
  if (!transaction) {
    co_return std::unexpected(std::move(transaction).error());
  }
  if (commit) {
    auto check = connection.prepare(R"sql(
SELECT COALESCE(MAX(sequence),0),
       COALESCE((SELECT json_extract(metadata_json,'$.oran_working_context.revision')
                 FROM sessions WHERE session_id=? AND agent_key=?),0)
FROM session_messages WHERE session_id=? AND agent_key=?
)sql");
    if (!check)
      co_return std::unexpected(check.error());
    if (auto bound = check->bind_all(key.session_id, key.agent_key, key.session_id, key.agent_key); !bound)
      co_return std::unexpected(bound.error());
    auto step = check->step();
    if (!step)
      co_return std::unexpected(step.error());
    auto count = check->column_int64(0);
    auto revision = check->column_int64(1);
    if (!count)
      co_return std::unexpected(count.error());
    if (!revision)
      co_return std::unexpected(revision.error());
    if (*count != commit->expected_messages || *revision != commit->expected_revision)
      co_return std::unexpected(core::Error{core::ErrorKind::conflict, "session changed during turn"});
  }
  {
    auto cached = writer->statement_cache().acquire(connection, kAppendMessageSql);
    if (!cached) {
      co_return std::unexpected(std::move(cached).error());
    }
    auto& statement = cached->statement();
    for (auto& message : messages) {
      auto inserted = insert_message(statement, key, std::move(message));
      if (!inserted) {
        co_return std::unexpected(std::move(inserted).error());
      }
      records.push_back(std::move(*inserted));
      if (auto reset = statement.reset(); !reset) {
        co_return std::unexpected(std::move(reset).error());
      }
    }
  }
  if (commit) {
    auto update = connection.prepare(R"sql(
UPDATE sessions SET metadata_json=json_set(metadata_json,'$.oran_working_context',json(?1))
WHERE session_id=?2 AND agent_key=?3 AND json_type(metadata_json)='object'
AND json_type(?1,'$.covered_sequence')='integer'
AND json_type(?1,'$.revision')='integer'
AND json_type(?1,'$.summary')='text'
AND length(CAST(json_extract(?1,'$.summary') AS BLOB)) <= 65536
AND (json_extract(?1,'$.covered_sequence')=0 OR length(json_extract(?1,'$.summary'))>0)
AND json_extract(?1,'$.covered_sequence') BETWEEN
  COALESCE(json_extract(metadata_json,'$.oran_working_context.covered_sequence'),0) AND
  (SELECT COALESCE(MAX(sequence),0) FROM session_messages WHERE session_id=?2 AND agent_key=?3)
AND (json_extract(?1,'$.revision') > ?4 OR
  (json_extract(?1,'$.revision') = ?4 AND
   json_extract(?1,'$.covered_sequence')=COALESCE(json_extract(metadata_json,'$.oran_working_context.covered_sequence'),0) AND
   json_extract(?1,'$.summary')=COALESCE(json_extract(metadata_json,'$.oran_working_context.summary'),'')))
RETURNING session_id
)sql");
    if (!update)
      co_return std::unexpected(update.error());
    if (auto bound =
            update->bind_all(commit->checkpoint_json, key.session_id, key.agent_key, commit->expected_revision);
        !bound)
      co_return std::unexpected(bound.error());
    auto step = update->step();
    if (!step)
      co_return std::unexpected(step.error());
    if (*step != StepResult::row)
      co_return std::unexpected(core::Error::storage("invalid session checkpoint"));
    if (auto done = update->expect_done("checkpoint"); !done)
      co_return std::unexpected(done.error());
  }
  if (auto committed = transaction->commit(); !committed) {
    co_return std::unexpected(std::move(committed).error());
  }
  co_return records;
}

async::Awaitable<core::Result<std::vector<SessionMessageRecord>>> SessionRepository::load_messages(SessionKey key) {
  if (auto valid = validate_key(key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kLoadMessagesSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_all(key.session_id, key.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<SessionMessageRecord> messages;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == StepResult::done) {
      break;
    }
    auto message = read_message_row(statement);
    if (!message) {
      co_return std::unexpected(message.error());
    }
    messages.push_back(std::move(*message));
  }

  co_return messages;
}

async::Awaitable<core::Result<std::vector<SessionMessageRecord>>>
SessionRepository::load_tail(SessionKey key, std::size_t max_messages, std::size_t max_bytes) {
  if (auto valid = validate_key(key); !valid)
    co_return std::unexpected(std::move(valid).error());
  if (max_messages == 0 || max_messages > 4096 || max_bytes == 0 || max_bytes > 16 * 1024 * 1024) {
    co_return std::unexpected(core::Error::invalid_argument("invalid conversation tail limit"));
  }
  auto reader = co_await pool_->acquire_reader();
  if (!reader)
    co_return std::unexpected(std::move(reader).error());
  auto cached = reader->statement_cache().acquire(reader->connection(), LOAD_TAIL_SQL);
  if (!cached)
    co_return std::unexpected(std::move(cached).error());
  auto& statement = cached->statement();
  if (auto bound = statement.bind_all(key.session_id, key.agent_key, static_cast<std::int64_t>(max_messages)); !bound)
    co_return std::unexpected(std::move(bound).error());
  std::size_t remaining = max_bytes;
  std::vector<SessionMessageRecord> rows;
  for (;;) {
    auto step = statement.step();
    if (!step)
      co_return std::unexpected(std::move(step).error());
    if (*step == StepResult::done)
      break;
    auto bytes = statement.column_int64(7);
    if (!bytes)
      co_return std::unexpected(std::move(bytes).error());
    if (*bytes < 0 || static_cast<std::uint64_t>(*bytes) > remaining)
      break;
    remaining -= static_cast<std::size_t>(*bytes);
    auto row = read_message_row(statement);
    if (!row)
      co_return std::unexpected(std::move(row).error());
    rows.push_back(std::move(*row));
  }
  std::ranges::reverse(rows);
  co_return rows;
}

async::Awaitable<core::Result<std::vector<SessionMessageRecord>>>
SessionRepository::load_after(SessionKey key,
                              std::int64_t after,
                              std::int64_t through,
                              std::size_t max_messages,
                              std::size_t max_bytes) {
  if (auto valid = validate_key(key); !valid)
    co_return std::unexpected(std::move(valid).error());
  if (after < 0 || through < after || max_messages == 0 || max_messages > 4096 || max_bytes == 0 ||
      max_bytes > 16 * 1024 * 1024) {
    co_return std::unexpected(core::Error::invalid_argument("invalid conversation tail limit"));
  }
  auto reader = co_await pool_->acquire_reader();
  if (!reader)
    co_return std::unexpected(std::move(reader).error());
  auto cached = reader->statement_cache().acquire(
      reader->connection(),
      R"sql(SELECT session_id,agent_key,sequence,role,content_json,metadata_json,created_at,
length(CAST(content_json AS BLOB))+length(CAST(metadata_json AS BLOB)) FROM session_messages
WHERE session_id=? AND agent_key=? AND sequence>? AND sequence<=? ORDER BY sequence LIMIT ?)sql");
  if (!cached)
    co_return std::unexpected(std::move(cached).error());
  auto& statement = cached->statement();
  if (auto bound =
          statement.bind_all(key.session_id, key.agent_key, after, through, static_cast<std::int64_t>(max_messages));
      !bound)
    co_return std::unexpected(std::move(bound).error());
  std::size_t remaining = max_bytes;
  std::vector<SessionMessageRecord> rows;
  for (;;) {
    auto step = statement.step();
    if (!step)
      co_return std::unexpected(std::move(step).error());
    if (*step == StepResult::done)
      break;
    auto bytes = statement.column_int64(7);
    if (!bytes)
      co_return std::unexpected(std::move(bytes).error());
    if (*bytes < 0 || static_cast<std::uint64_t>(*bytes) > remaining) {
      if (rows.empty())
        co_return std::unexpected(core::Error::invalid_argument("session row exceeds context page budget"));
      break;
    }
    remaining -= static_cast<std::size_t>(*bytes);
    auto row = read_message_row(statement);
    if (!row)
      co_return std::unexpected(std::move(row).error());
    if (row->sequence != after + static_cast<std::int64_t>(rows.size()) + 1)
      co_return std::unexpected(core::Error::storage("session history has a sequence gap"));
    rows.push_back(std::move(*row));
  }

  co_return rows;
}

async::Awaitable<core::Result<std::optional<SessionRecord>>> SessionRepository::get_session(SessionKey key) {
  if (auto valid = validate_key(key); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kGetSessionSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_all(key.session_id, key.agent_key); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == StepResult::done) {
    co_return std::optional<SessionRecord>{};
  }

  auto session = read_session_row(statement);
  if (!session) {
    co_return std::unexpected(session.error());
  }
  if (auto done = statement.expect_done("get_session"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::optional<SessionRecord>{std::move(*session)};
}

}  // namespace orangutan::storage
