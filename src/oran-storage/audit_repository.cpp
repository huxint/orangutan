#include <oran/storage/audit_repository.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/storage/migrations.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::storage {

namespace {

constexpr std::string_view kAppendEventSql = R"sql(
INSERT INTO audit_events(
  event_kind, scope_key, agent_key, tool_name, identity, verdict, outcome, reason,
  input_hash_hex, parent_turn_id, metadata_json, created_at
)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
RETURNING id, created_at
)sql";

[[nodiscard]] core::Error invalid_field(std::string field) {
  return core::Error::invalid_argument("audit repository field must not be empty").with("field", std::move(field));
}

[[nodiscard]] core::Result<void> validate_append_request(const AppendAuditEventRequest& request) {
  if (request.event_kind.empty()) {
    return std::unexpected(invalid_field("event_kind"));
  }
  if (request.scope_key.empty()) {
    return std::unexpected(invalid_field("scope_key"));
  }
  if (request.agent_key.empty()) {
    return std::unexpected(invalid_field("agent_key"));
  }
  if (request.tool_name.empty()) {
    return std::unexpected(invalid_field("tool_name"));
  }
  if (request.identity.empty()) {
    return std::unexpected(invalid_field("identity"));
  }
  if (request.verdict.empty()) {
    return std::unexpected(invalid_field("verdict"));
  }
  if (request.outcome.empty()) {
    return std::unexpected(invalid_field("outcome"));
  }
  if (request.reason.empty()) {
    return std::unexpected(invalid_field("reason"));
  }
  if (request.metadata_json.empty()) {
    return std::unexpected(invalid_field("metadata_json"));
  }
  if (request.parent_turn_id.has_value() && core::is_zero_turn_id(*request.parent_turn_id)) {
    return std::unexpected(invalid_field("parent_turn_id"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_list_options(const ListAuditEventsOptions& options) {
  if (options.scope_key.empty()) {
    return std::unexpected(invalid_field("scope_key"));
  }
  if (options.limit == 0) {
    return std::unexpected(core::Error::invalid_argument("audit list limit must be greater than zero"));
  }
  if (options.limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(core::Error::invalid_argument("audit list limit is too large"));
  }
  return {};
}

[[nodiscard]] core::Result<std::optional<core::TurnId>>
optional_turn_id(Statement& statement, int index, std::string_view field) {
  auto value = statement.column_blob(index);
  if (!value) {
    return std::unexpected(value.error().with("field", std::string{field}));
  }
  if (!*value) {
    return std::optional<core::TurnId>{};
  }
  if ((*value)->size() != core::TurnId{}.size()) {
    return std::unexpected(core::Error::storage("audit repository row has invalid turn id length")
                               .with("field", std::string{field})
                               .with("size", std::to_string((*value)->size())));
  }
  core::TurnId id{};
  std::ranges::copy(**std::move(value), id.begin());
  return std::optional<core::TurnId>{id};
}

[[nodiscard]] core::Result<AuditEventRecord> read_event_row(Statement& statement) {
  auto id = statement.column_int64(0);
  if (!id) {
    return std::unexpected(id.error().with("field", "id"));
  }
  auto event_kind = statement.required_text(1, "event_kind");
  if (!event_kind) {
    return std::unexpected(event_kind.error());
  }
  auto scope_key = statement.required_text(2, "scope_key");
  if (!scope_key) {
    return std::unexpected(scope_key.error());
  }
  auto agent_key = statement.required_text(3, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  auto tool_name = statement.required_text(4, "tool_name");
  if (!tool_name) {
    return std::unexpected(tool_name.error());
  }
  auto identity = statement.required_text(5, "identity");
  if (!identity) {
    return std::unexpected(identity.error());
  }
  auto verdict = statement.required_text(6, "verdict");
  if (!verdict) {
    return std::unexpected(verdict.error());
  }
  auto outcome = statement.required_text(7, "outcome");
  if (!outcome) {
    return std::unexpected(outcome.error());
  }
  auto reason = statement.required_text(8, "reason");
  if (!reason) {
    return std::unexpected(reason.error());
  }
  auto input_hash = statement.column_text(9);
  if (!input_hash) {
    return std::unexpected(input_hash.error());
  }
  auto parent_turn_id = optional_turn_id(statement, 10, "parent_turn_id");
  if (!parent_turn_id) {
    return std::unexpected(parent_turn_id.error());
  }
  auto metadata_json = statement.required_text(11, "metadata_json");
  if (!metadata_json) {
    return std::unexpected(metadata_json.error());
  }
  auto created_at = statement.required_text(12, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }

  return AuditEventRecord{
      .id = *id,
      .event_kind = std::move(*event_kind),
      .scope_key = std::move(*scope_key),
      .agent_key = std::move(*agent_key),
      .tool_name = std::move(*tool_name),
      .identity = std::move(*identity),
      .verdict = std::move(*verdict),
      .outcome = std::move(*outcome),
      .reason = std::move(*reason),
      .input_hash_hex = std::move(*input_hash),
      .parent_turn_id = std::move(*parent_turn_id),
      .metadata_json = std::move(*metadata_json),
      .created_at = std::move(*created_at),
  };
}

/// Build the dynamic SELECT for `list_events`. The SQL grows secondary
/// filters as bind parameters so re-used statement cache entries can hit
/// for repeat callers with the same shape.
[[nodiscard]] std::string build_list_sql(const ListAuditEventsOptions& options) {
  std::string sql{"SELECT id, event_kind, scope_key, agent_key, tool_name, identity, verdict, outcome, reason, "
                  "input_hash_hex, parent_turn_id, metadata_json, created_at "
                  "FROM audit_events WHERE scope_key = ?"};
  if (!options.agent_key.empty()) {
    sql += " AND agent_key = ?";
  }
  if (!options.tool_name.empty()) {
    sql += " AND tool_name = ?";
  }
  if (!options.event_kind.empty()) {
    sql += " AND event_kind = ?";
  }
  if (!options.outcome.empty()) {
    sql += " AND outcome = ?";
  }
  sql += " ORDER BY id DESC LIMIT ?";
  return sql;
}

}  // namespace

AuditRepository::AuditRepository(Pool& pool, AuditRepositoryOptions options) noexcept
    : pool_{&pool}, options_{std::move(options)} {}

async::Awaitable<core::Result<MigrationReport>> AuditRepository::migrate() {
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  if (options_.migrations_directory.empty()) {
    auto report = run_migrations(writer->connection(), built_in_audit_migrations());
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

async::Awaitable<core::Result<AuditEventRecord>> AuditRepository::append_event(AppendAuditEventRequest request) {
  if (auto valid = validate_append_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kAppendEventSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();

  const auto input_hash_hex = request.input_hash_hex.empty()
                                  ? std::optional<std::string_view>{}
                                  : std::optional<std::string_view>{request.input_hash_hex};
  if (auto bound = statement.bind_all(request.event_kind, request.scope_key, request.agent_key, request.tool_name,
                                      request.identity, request.verdict, request.outcome, request.reason,
                                      input_hash_hex, request.parent_turn_id, request.metadata_json);
      !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != StepResult::row) {
    co_return std::unexpected(core::Error::storage("audit event insert returned no row"));
  }

  auto id = statement.column_int64(0);
  if (!id) {
    co_return std::unexpected(id.error().with("field", "id"));
  }
  auto created_at = statement.required_text(1, "created_at");
  if (!created_at) {
    co_return std::unexpected(created_at.error());
  }
  if (auto done = statement.expect_done("append_event"); !done) {
    co_return std::unexpected(done.error());
  }

  auto record = AuditEventRecord{
      .id = *id,
      .event_kind = std::move(request.event_kind),
      .scope_key = std::move(request.scope_key),
      .agent_key = std::move(request.agent_key),
      .tool_name = std::move(request.tool_name),
      .identity = std::move(request.identity),
      .verdict = std::move(request.verdict),
      .outcome = std::move(request.outcome),
      .reason = std::move(request.reason),
      .input_hash_hex = {},
      .parent_turn_id = std::move(request.parent_turn_id),
      .metadata_json = std::move(request.metadata_json),
      .created_at = std::move(*created_at),
  };
  if (!request.input_hash_hex.empty()) {
    record.input_hash_hex = std::move(request.input_hash_hex);
  }
  co_return record;
}

async::Awaitable<core::Result<std::vector<AuditEventRecord>>>
AuditRepository::list_events(ListAuditEventsOptions options) {
  if (auto valid = validate_list_options(options); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  const auto sql = build_list_sql(options);
  auto cached = reader->statement_cache().acquire(reader->connection(), sql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();

  int index = 1;
  if (auto bound = statement.bind_text(index++, options.scope_key); !bound) {
    co_return std::unexpected(bound.error());
  }
  if (!options.agent_key.empty()) {
    if (auto bound = statement.bind_text(index++, options.agent_key); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (!options.tool_name.empty()) {
    if (auto bound = statement.bind_text(index++, options.tool_name); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (!options.event_kind.empty()) {
    if (auto bound = statement.bind_text(index++, options.event_kind); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (!options.outcome.empty()) {
    if (auto bound = statement.bind_text(index++, options.outcome); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (auto bound = statement.bind_int64(index, static_cast<std::int64_t>(options.limit)); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<AuditEventRecord> events;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == StepResult::done) {
      break;
    }
    auto event = read_event_row(statement);
    if (!event) {
      co_return std::unexpected(event.error());
    }
    events.push_back(std::move(*event));
  }

  co_return events;
}

}  // namespace orangutan::storage
