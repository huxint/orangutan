#include <oran/storage/session_repository.hpp>

#include <array>
#include <asio/this_coro.hpp>
#include <string_view>

#include <oran/core/error.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>

namespace orangutan::storage {

async::Awaitable<core::Result<void>> SessionRepository::backup_to(std::string destination) {
  auto reader = co_await pool_->acquire_reader();
  if (!reader)
    co_return std::unexpected(reader.error());
  co_return reader->connection().backup_to(destination);
}

async::Awaitable<core::Result<void>>
SessionRepository::import_session(std::string source_path, SessionKey source, SessionKey destination) {
  if (source_path.empty() || source.session_id.empty() || source.agent_key.empty() || destination.session_id.empty() ||
      destination.agent_key.empty())
    co_return std::unexpected(
        core::Error::invalid_argument("import requires explicit source and destination identities"));
  auto input = Connection::open({.path = std::move(source_path), .mode = OpenMode::read_only, .enable_wal = false});
  if (!input)
    co_return std::unexpected(input.error());
  // A read transaction pins one source snapshot for all three tables.
  if (auto begun = input->execute("BEGIN"); !begun)
    co_return std::unexpected(begun.error());
  auto version = input->query("SELECT MAX(version) FROM schema_versions");
  if (!version)
    co_return std::unexpected(version.error());
  if (version->rows.size() != 1 || version->rows[0].values[0] != "2")
    co_return std::unexpected(core::Error::invalid_argument("unsupported session import schema"));
  auto writer = co_await pool_->acquire_writer();
  if (!writer)
    co_return std::unexpected(writer.error());
  auto& output = writer->connection();
  auto transaction = Transaction::begin(output);
  if (!transaction)
    co_return std::unexpected(transaction.error());
  auto existing = output.prepare(R"sql(
SELECT 1 FROM sessions WHERE session_id=?1 AND agent_key=?2
UNION ALL SELECT 1 FROM session_messages WHERE session_id=?1 AND agent_key=?2
UNION ALL SELECT 1 FROM session_skill_activations WHERE session_id=?1 AND agent_key=?2 LIMIT 1
)sql");
  if (!existing)
    co_return std::unexpected(existing.error());
  if (auto bound = existing->bind_all(destination.session_id, destination.agent_key); !bound)
    co_return std::unexpected(bound.error());
  auto found = existing->step();
  if (!found)
    co_return std::unexpected(found.error());
  if (*found != StepResult::done)
    co_return std::unexpected(core::Error{core::ErrorKind::conflict, "session import destination already exists"});

  struct CopyTable {
    std::string_view select;
    std::string_view insert;
  };
  const auto tables = std::array{
      CopyTable{
          "SELECT sequence,role,content_json,metadata_json,created_at FROM session_messages WHERE session_id=? AND "
          "agent_key=? ORDER BY sequence",
          "INSERT INTO session_messages(session_id,agent_key,sequence,role,content_json,metadata_json,created_at) "
          "VALUES(?,?,?,?,?,?,?)"},
      CopyTable{"SELECT skill_name,active,created_at,updated_at FROM session_skill_activations WHERE session_id=? AND "
                "agent_key=?",
                "INSERT INTO session_skill_activations(session_id,agent_key,skill_name,active,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?)"},
      CopyTable{"SELECT title,metadata_json,created_at,updated_at FROM sessions WHERE session_id=? AND agent_key=?",
                "INSERT INTO sessions(session_id,agent_key,title,metadata_json,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?) ON CONFLICT(session_id,agent_key) DO UPDATE SET "
                "title=excluded.title,metadata_json=excluded.metadata_json,created_at=excluded.created_at,updated_at="
                "excluded.updated_at"}};
  bool session_found = false;
  for (std::size_t table = 0; table < tables.size(); ++table) {
    auto read = input->prepare(tables[table].select);
    auto write = output.prepare(tables[table].insert);
    if (!read)
      co_return std::unexpected(read.error());
    if (!write)
      co_return std::unexpected(write.error());
    if (auto bound = read->bind_all(source.session_id, source.agent_key); !bound)
      co_return std::unexpected(bound.error());
    auto columns = read->column_count();
    if (!columns)
      co_return std::unexpected(columns.error());
    for (;;) {
      if (auto cancellation = co_await asio::this_coro::cancellation_state;
          cancellation.cancelled() != asio::cancellation_type::none)
        co_return std::unexpected(core::Error::cancelled());
      auto step = read->step();
      if (!step)
        co_return std::unexpected(step.error());
      if (*step == StepResult::done)
        break;
      if (table == 2)
        session_found = true;
      if (auto bound = write->bind_all(destination.session_id, destination.agent_key); !bound)
        co_return std::unexpected(bound.error());
      for (int column = 0; column < *columns; ++column) {
        auto value = read->column_text(column);
        if (!value)
          co_return std::unexpected(value.error());
        auto bound = *value ? write->bind_text(column + 3, **value) : write->bind_null(column + 3);
        if (!bound)
          co_return std::unexpected(bound.error());
      }
      if (auto done = write->expect_done("import session"); !done)
        co_return std::unexpected(done.error());
      if (auto reset = write->reset(); !reset)
        co_return std::unexpected(reset.error());
    }
  }
  if (!session_found)
    co_return std::unexpected(core::Error::invalid_argument("session import source not found"));
  co_return transaction->commit();
}

}  // namespace orangutan::storage
