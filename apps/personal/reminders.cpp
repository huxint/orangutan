#include "reminders.hpp"

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/storage/migrations.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::personal {
namespace {

constexpr std::string_view kMigrationSql = R"sql(
CREATE TABLE IF NOT EXISTS personal_reminders (
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  due_at_ms INTEGER NOT NULL,
  completed INTEGER NOT NULL DEFAULT 0,
  notified INTEGER NOT NULL DEFAULT 0,
  created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_personal_reminders_due
  ON personal_reminders(completed, notified, due_at_ms);
)sql";

constexpr std::string_view kListSql = R"sql(
SELECT id, title, due_at_ms, completed, notified, created_at_ms
FROM personal_reminders
ORDER BY completed ASC, due_at_ms ASC, created_at_ms ASC
LIMIT 200
)sql";

constexpr std::string_view kInsertSql = R"sql(
INSERT INTO personal_reminders(id, title, due_at_ms, completed, notified, created_at_ms)
VALUES (?, ?, ?, ?, ?, ?)
)sql";

constexpr std::string_view kCompleteSql = R"sql(
UPDATE personal_reminders
SET completed = ?, notified = CASE WHEN ? = 1 THEN 1 ELSE notified END
WHERE id = ?
)sql";

constexpr std::string_view kDeleteSql = R"sql(
DELETE FROM personal_reminders WHERE id = ?
)sql";

constexpr std::string_view kSelectDueSql = R"sql(
SELECT id, title, due_at_ms, completed, notified, created_at_ms
FROM personal_reminders
WHERE completed = 0 AND notified = 0 AND due_at_ms <= ?
ORDER BY due_at_ms ASC
LIMIT 32
)sql";

constexpr std::string_view kMarkNotifiedSql = R"sql(
UPDATE personal_reminders SET notified = 1 WHERE id = ?
)sql";

[[nodiscard]] core::Result<Reminder> read_row(storage::Statement& stmt) {
  auto id = stmt.required_text(0, "id");
  if (!id) {
    return std::unexpected(std::move(id).error());
  }
  auto title = stmt.required_text(1, "title");
  if (!title) {
    return std::unexpected(std::move(title).error());
  }
  auto due_at = stmt.column_int64(2);
  if (!due_at) {
    return std::unexpected(std::move(due_at).error());
  }
  auto completed = stmt.column_int64(3);
  if (!completed) {
    return std::unexpected(std::move(completed).error());
  }
  auto notified = stmt.column_int64(4);
  if (!notified) {
    return std::unexpected(std::move(notified).error());
  }
  auto created_at = stmt.column_int64(5);
  if (!created_at) {
    return std::unexpected(std::move(created_at).error());
  }
  return Reminder{
      .id = std::move(*id),
      .title = std::move(*title),
      .due_at_ms = *due_at,
      .completed = *completed != 0,
      .notified = *notified != 0,
      .created_at_ms = *created_at,
  };
}

}  // namespace

ReminderStore::ReminderStore(storage::Pool& pool) noexcept : pool_{&pool} {}

async::Awaitable<core::Result<void>> ReminderStore::migrate() {
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  const std::array<storage::Migration, 1> migrations{
      storage::Migration{
          .version = 1,
          .name = "personal-reminders-initial",
          .sql = std::string{kMigrationSql},
      },
  };
  auto report = storage::run_migrations(writer->connection(), migrations);
  if (!report) {
    co_return std::unexpected(std::move(report).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<std::vector<Reminder>>> ReminderStore::list() {
  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(std::move(reader).error());
  }
  auto cached = reader->statement_cache().acquire(reader->connection(), kListSql);
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  auto& stmt = cached->statement();
  std::vector<Reminder> out;
  while (true) {
    auto step = stmt.step();
    if (!step) {
      co_return std::unexpected(std::move(step).error());
    }
    if (*step == storage::StepResult::done) {
      break;
    }
    auto row = read_row(stmt);
    if (!row) {
      co_return std::unexpected(std::move(row).error());
    }
    out.push_back(std::move(*row));
  }
  co_return out;
}

async::Awaitable<core::Result<Reminder>> ReminderStore::add(Reminder reminder) {
  if (reminder.id.empty() || reminder.title.empty() || reminder.title.size() > 240U) {
    co_return std::unexpected(core::Error::invalid_argument("reminder id and title (1-240 bytes) are required"));
  }
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto tx = storage::Transaction::begin(writer->connection());
  if (!tx) {
    co_return std::unexpected(std::move(tx).error());
  }
  auto cached = writer->statement_cache().acquire(writer->connection(), kInsertSql);
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  auto& stmt = cached->statement();
  if (auto bound = stmt.bind_all(reminder.id,
                                 reminder.title,
                                 reminder.due_at_ms,
                                 static_cast<std::int64_t>(reminder.completed ? 1 : 0),
                                 static_cast<std::int64_t>(reminder.notified ? 1 : 0),
                                 reminder.created_at_ms);
      !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  if (auto done = stmt.expect_done("insert_reminder"); !done) {
    co_return std::unexpected(std::move(done).error());
  }
  if (auto committed = tx->commit(); !committed) {
    co_return std::unexpected(std::move(committed).error());
  }
  co_return reminder;
}

async::Awaitable<core::Result<void>> ReminderStore::mark_completed(std::string id, bool completed) {
  if (id.empty()) {
    co_return std::unexpected(core::Error::invalid_argument("reminder id must not be empty"));
  }
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto tx = storage::Transaction::begin(writer->connection());
  if (!tx) {
    co_return std::unexpected(std::move(tx).error());
  }
  auto cached = writer->statement_cache().acquire(writer->connection(), kCompleteSql);
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  const std::int64_t flag = completed ? 1 : 0;
  if (auto bound = cached->statement().bind_all(flag, flag, id); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  if (auto done = cached->statement().expect_done("complete_reminder"); !done) {
    co_return std::unexpected(std::move(done).error());
  }
  if (auto committed = tx->commit(); !committed) {
    co_return std::unexpected(std::move(committed).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> ReminderStore::remove(std::string id) {
  if (id.empty()) {
    co_return std::unexpected(core::Error::invalid_argument("reminder id must not be empty"));
  }
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto tx = storage::Transaction::begin(writer->connection());
  if (!tx) {
    co_return std::unexpected(std::move(tx).error());
  }
  auto cached = writer->statement_cache().acquire(writer->connection(), kDeleteSql);
  if (!cached) {
    co_return std::unexpected(std::move(cached).error());
  }
  if (auto bound = cached->statement().bind_all(id); !bound) {
    co_return std::unexpected(std::move(bound).error());
  }
  if (auto done = cached->statement().expect_done("delete_reminder"); !done) {
    co_return std::unexpected(std::move(done).error());
  }
  if (auto committed = tx->commit(); !committed) {
    co_return std::unexpected(std::move(committed).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<std::vector<Reminder>>> ReminderStore::claim_due(std::int64_t now_ms) {
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(std::move(writer).error());
  }
  auto tx = storage::Transaction::begin(writer->connection());
  if (!tx) {
    co_return std::unexpected(std::move(tx).error());
  }
  std::vector<Reminder> due;
  {
    auto cached = writer->statement_cache().acquire(writer->connection(), kSelectDueSql);
    if (!cached) {
      co_return std::unexpected(std::move(cached).error());
    }
    auto& stmt = cached->statement();
    if (auto bound = stmt.bind_all(now_ms); !bound) {
      co_return std::unexpected(std::move(bound).error());
    }
    while (true) {
      auto step = stmt.step();
      if (!step) {
        co_return std::unexpected(std::move(step).error());
      }
      if (*step == storage::StepResult::done) {
        break;
      }
      auto row = read_row(stmt);
      if (!row) {
        co_return std::unexpected(std::move(row).error());
      }
      row->notified = true;
      due.push_back(std::move(*row));
    }
  }
  if (!due.empty()) {
    auto cached = writer->statement_cache().acquire(writer->connection(), kMarkNotifiedSql);
    if (!cached) {
      co_return std::unexpected(std::move(cached).error());
    }
    auto& stmt = cached->statement();
    for (const auto& item : due) {
      if (auto reset = stmt.reset(); !reset) {
        co_return std::unexpected(std::move(reset).error());
      }
      if (auto bound = stmt.bind_all(item.id); !bound) {
        co_return std::unexpected(std::move(bound).error());
      }
      if (auto done = stmt.expect_done("mark_reminder_notified"); !done) {
        co_return std::unexpected(std::move(done).error());
      }
    }
  }
  if (auto committed = tx->commit(); !committed) {
    co_return std::unexpected(std::move(committed).error());
  }
  co_return due;
}

}  // namespace orangutan::personal
