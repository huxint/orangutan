#include <oran/storage/trace_repository.hpp>

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
#include <oran/storage/migrations.hpp>
#include <oran/storage/pool.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/storage/statement_cache.hpp>

namespace orangutan::storage {

namespace {

constexpr std::string_view kAppendTurnSql = R"sql(
INSERT INTO trace_turns(
  turn_id, parent_turn_id, session_id, agent_key, origin, route_profile,
  route_model, started_at_ns, finished_at_ns, stop_reason, iteration_count,
  prompt_prefix_hash, prompt_prefix_bytes, active_catalog_hash,
  deferred_catalog_hash, cache_creation_tokens, cache_read_tokens,
  input_tokens, output_tokens, cost_estimate_usd, cancellation_phase,
  context_json, schema_version
)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?, ?, ?, ?)
RETURNING turn_id, parent_turn_id, session_id, agent_key, origin, route_profile,
  route_model, started_at_ns, finished_at_ns, stop_reason, iteration_count,
  prompt_prefix_hash, prompt_prefix_bytes, active_catalog_hash,
  cache_creation_tokens, cache_read_tokens,
  input_tokens, output_tokens, cost_estimate_usd, cancellation_phase,
  context_json, schema_version
)sql";

constexpr std::string_view kGetTurnSql = R"sql(
SELECT turn_id, parent_turn_id, session_id, agent_key, origin, route_profile,
  route_model, started_at_ns, finished_at_ns, stop_reason, iteration_count,
  prompt_prefix_hash, prompt_prefix_bytes, active_catalog_hash,
  cache_creation_tokens, cache_read_tokens,
  input_tokens, output_tokens, cost_estimate_usd, cancellation_phase,
  context_json, schema_version
FROM trace_turns
WHERE turn_id = ?
)sql";

[[nodiscard]] core::Error invalid_field(std::string field) {
  return core::Error::invalid_argument("trace repository field is invalid").with("field", std::move(field));
}

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) noexcept {
  return std::as_bytes(std::span{text.data(), text.size()});
}

[[nodiscard]] std::int64_t to_sql_hash(std::uint64_t value) noexcept {
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::uint64_t from_sql_hash(std::int64_t value) noexcept {
  return static_cast<std::uint64_t>(value);
}

[[nodiscard]] core::Result<void> validate_non_negative(std::int64_t value, std::string field) {
  if (value < 0) {
    return std::unexpected(invalid_field(std::move(field)).with("reason", "negative"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_append_request(const AppendTraceTurnRequest& request) {
  if (core::is_zero_turn_id(request.turn_id)) {
    return std::unexpected(invalid_field("turn_id").with("reason", "zero_id"));
  }
  if (core::is_zero_turn_id(request.session_id)) {
    return std::unexpected(invalid_field("session_id").with("reason", "zero_id"));
  }
  if (request.parent_turn_id.has_value() && core::is_zero_turn_id(*request.parent_turn_id)) {
    return std::unexpected(invalid_field("parent_turn_id").with("reason", "zero_id"));
  }
  if (request.agent_key.empty()) {
    return std::unexpected(invalid_field("agent_key").with("reason", "empty"));
  }
  if (request.origin.empty()) {
    return std::unexpected(invalid_field("origin").with("reason", "empty"));
  }
  if (request.route_profile.empty()) {
    return std::unexpected(invalid_field("route_profile").with("reason", "empty"));
  }
  if (request.route_model.empty()) {
    return std::unexpected(invalid_field("route_model").with("reason", "empty"));
  }
  if (request.stop_reason.empty()) {
    return std::unexpected(invalid_field("stop_reason").with("reason", "empty"));
  }
  if (request.context_json.empty()) {
    return std::unexpected(invalid_field("context_json").with("reason", "empty"));
  }
  if (request.iteration_count <= 0) {
    return std::unexpected(invalid_field("iteration_count").with("reason", "not_positive"));
  }
  if (request.schema_version <= 0) {
    return std::unexpected(invalid_field("schema_version").with("reason", "not_positive"));
  }
  if (auto valid = validate_non_negative(request.started_at_ns, "started_at_ns"); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_non_negative(request.finished_at_ns, "finished_at_ns"); !valid) {
    return std::unexpected(valid.error());
  }
  if (request.finished_at_ns < request.started_at_ns) {
    return std::unexpected(invalid_field("finished_at_ns").with("reason", "before_started_at"));
  }
  if (auto valid = validate_non_negative(request.prompt_prefix_bytes, "prompt_prefix_bytes"); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_non_negative(request.cache_creation_tokens, "cache_creation_tokens"); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_non_negative(request.cache_read_tokens, "cache_read_tokens"); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_non_negative(request.input_tokens, "input_tokens"); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_non_negative(request.output_tokens, "output_tokens"); !valid) {
    return std::unexpected(valid.error());
  }
  return {};
}

[[nodiscard]] core::Result<std::int64_t> checked_limit(std::size_t limit) {
  if (limit == 0) {
    return std::unexpected(core::Error::invalid_argument("trace list limit must be greater than zero"));
  }
  if (limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(core::Error::invalid_argument("trace list limit is too large"));
  }
  return static_cast<std::int64_t>(limit);
}

[[nodiscard]] core::Result<TraceId> required_trace_id(Statement& statement, int index, std::string_view field) {
  auto value = statement.column_blob(index);
  if (!value) {
    return std::unexpected(value.error().with("field", std::string{field}));
  }
  if (!*value) {
    return std::unexpected(
        core::Error::storage("trace repository row has null required id").with("field", std::string{field}));
  }
  if ((*value)->size() != TraceId{}.size()) {
    return std::unexpected(core::Error::storage("trace repository row has invalid id length")
                               .with("field", std::string{field})
                               .with("size", std::to_string((*value)->size())));
  }
  TraceId id{};
  std::ranges::copy(**std::move(value), id.begin());
  return id;
}

[[nodiscard]] core::Result<std::optional<TraceId>> optional_trace_id(Statement& statement, int index) {
  auto value = statement.column_blob(index);
  if (!value) {
    return std::unexpected(value.error());
  }
  if (!*value) {
    return std::optional<TraceId>{};
  }
  if ((*value)->size() != TraceId{}.size()) {
    return std::unexpected(core::Error::storage("trace repository row has invalid optional id length")
                               .with("size", std::to_string((*value)->size())));
  }
  TraceId id{};
  std::ranges::copy(**std::move(value), id.begin());
  return std::optional<TraceId>{id};
}

[[nodiscard]] core::Result<std::string> required_blob_text(Statement& statement, int index, std::string_view field) {
  auto value = statement.column_blob(index);
  if (!value) {
    return std::unexpected(value.error().with("field", std::string{field}));
  }
  if (!*value) {
    return std::unexpected(
        core::Error::storage("trace repository row has null required blob").with("field", std::string{field}));
  }
  auto& bytes = **value;
  return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] core::Result<TraceTurnRecord> read_turn_row(Statement& statement) {
  auto turn_id = required_trace_id(statement, 0, "turn_id");
  if (!turn_id) {
    return std::unexpected(turn_id.error());
  }
  auto parent_turn_id = optional_trace_id(statement, 1);
  if (!parent_turn_id) {
    return std::unexpected(parent_turn_id.error());
  }
  auto session_id = required_trace_id(statement, 2, "session_id");
  if (!session_id) {
    return std::unexpected(session_id.error());
  }
  auto agent_key = statement.required_text(3, "agent_key");
  if (!agent_key) {
    return std::unexpected(agent_key.error());
  }
  auto origin = statement.required_text(4, "origin");
  if (!origin) {
    return std::unexpected(origin.error());
  }
  auto route_profile = statement.required_text(5, "route_profile");
  if (!route_profile) {
    return std::unexpected(route_profile.error());
  }
  auto route_model = statement.required_text(6, "route_model");
  if (!route_model) {
    return std::unexpected(route_model.error());
  }
  auto started_at_ns = statement.column_int64(7);
  if (!started_at_ns) {
    return std::unexpected(started_at_ns.error().with("field", "started_at_ns"));
  }
  auto finished_at_ns = statement.column_int64(8);
  if (!finished_at_ns) {
    return std::unexpected(finished_at_ns.error().with("field", "finished_at_ns"));
  }
  auto stop_reason = statement.required_text(9, "stop_reason");
  if (!stop_reason) {
    return std::unexpected(stop_reason.error());
  }
  auto iteration_count = statement.column_int64(10);
  if (!iteration_count) {
    return std::unexpected(iteration_count.error().with("field", "iteration_count"));
  }
  auto prompt_prefix_hash = statement.column_int64(11);
  if (!prompt_prefix_hash) {
    return std::unexpected(prompt_prefix_hash.error().with("field", "prompt_prefix_hash"));
  }
  auto prompt_prefix_bytes = statement.column_int64(12);
  if (!prompt_prefix_bytes) {
    return std::unexpected(prompt_prefix_bytes.error().with("field", "prompt_prefix_bytes"));
  }
  auto active_catalog_hash = statement.column_int64(13);
  if (!active_catalog_hash) {
    return std::unexpected(active_catalog_hash.error().with("field", "active_catalog_hash"));
  }
  auto cache_creation_tokens = statement.column_int64(14);
  if (!cache_creation_tokens) {
    return std::unexpected(cache_creation_tokens.error().with("field", "cache_creation_tokens"));
  }
  auto cache_read_tokens = statement.column_int64(15);
  if (!cache_read_tokens) {
    return std::unexpected(cache_read_tokens.error().with("field", "cache_read_tokens"));
  }
  auto input_tokens = statement.column_int64(16);
  if (!input_tokens) {
    return std::unexpected(input_tokens.error().with("field", "input_tokens"));
  }
  auto output_tokens = statement.column_int64(17);
  if (!output_tokens) {
    return std::unexpected(output_tokens.error().with("field", "output_tokens"));
  }
  auto cost_estimate_usd = statement.column_double(18);
  if (!cost_estimate_usd) {
    return std::unexpected(cost_estimate_usd.error().with("field", "cost_estimate_usd"));
  }
  auto cancellation_phase = statement.column_text(19);
  if (!cancellation_phase) {
    return std::unexpected(cancellation_phase.error());
  }
  auto context_json = required_blob_text(statement, 20, "context_json");
  if (!context_json) {
    return std::unexpected(context_json.error());
  }
  auto schema_version = statement.column_int64(21);
  if (!schema_version) {
    return std::unexpected(schema_version.error().with("field", "schema_version"));
  }

  return TraceTurnRecord{
      .turn_id = *turn_id,
      .parent_turn_id = std::move(*parent_turn_id),
      .session_id = *session_id,
      .agent_key = std::move(*agent_key),
      .origin = std::move(*origin),
      .route_profile = std::move(*route_profile),
      .route_model = std::move(*route_model),
      .started_at_ns = *started_at_ns,
      .finished_at_ns = *finished_at_ns,
      .stop_reason = std::move(*stop_reason),
      .iteration_count = *iteration_count,
      .prompt_prefix_hash = from_sql_hash(*prompt_prefix_hash),
      .prompt_prefix_bytes = *prompt_prefix_bytes,
      .active_catalog_hash = from_sql_hash(*active_catalog_hash),
      .cache_creation_tokens = *cache_creation_tokens,
      .cache_read_tokens = *cache_read_tokens,
      .input_tokens = *input_tokens,
      .output_tokens = *output_tokens,
      .cost_estimate_usd = *cost_estimate_usd,
      .cancellation_phase = std::move(*cancellation_phase),
      .context_json = std::move(*context_json),
      .schema_version = *schema_version,
  };
}

[[nodiscard]] std::string build_list_sql(const ListTraceTurnsOptions& options) {
  std::string sql{"SELECT turn_id, parent_turn_id, session_id, agent_key, origin, route_profile, "
                  "route_model, started_at_ns, finished_at_ns, stop_reason, iteration_count, "
                  "prompt_prefix_hash, prompt_prefix_bytes, active_catalog_hash, "
                  "cache_creation_tokens, cache_read_tokens, "
                  "input_tokens, output_tokens, cost_estimate_usd, cancellation_phase, "
                  "context_json, schema_version FROM trace_turns"};
  bool has_where = false;
  if (options.session_id.has_value()) {
    sql += " WHERE session_id = ?";
    has_where = true;
  }
  if (!options.agent_key.empty()) {
    sql += has_where ? " AND agent_key = ?" : " WHERE agent_key = ?";
  }
  sql += " ORDER BY started_at_ns DESC, turn_id ASC LIMIT ?";
  return sql;
}

}  // namespace

TraceRepository::TraceRepository(Pool& pool, TraceRepositoryOptions options) noexcept
    : pool_{&pool}, options_{std::move(options)} {}

async::Awaitable<core::Result<MigrationReport>> TraceRepository::migrate() {
  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  if (options_.migrations_directory.empty()) {
    auto report = run_migrations(writer->connection(), built_in_trace_migrations());
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

async::Awaitable<core::Result<TraceTurnRecord>> TraceRepository::append_turn(AppendTraceTurnRequest request) {
  if (auto valid = validate_append_request(request); !valid) {
    co_return std::unexpected(valid.error());
  }

  auto writer = co_await pool_->acquire_writer();
  if (!writer) {
    co_return std::unexpected(writer.error());
  }

  auto cached = writer->statement_cache().acquire(writer->connection(), kAppendTurnSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();

  if (auto bound = statement.bind_all(
          request.turn_id, request.parent_turn_id, request.session_id, request.agent_key, request.origin,
          request.route_profile, request.route_model, request.started_at_ns, request.finished_at_ns,
          request.stop_reason, request.iteration_count, to_sql_hash(request.prompt_prefix_hash),
          request.prompt_prefix_bytes, to_sql_hash(request.active_catalog_hash), request.cache_creation_tokens,
          request.cache_read_tokens,
          request.input_tokens, request.output_tokens, request.cost_estimate_usd, request.cancellation_phase,
          bytes_of(request.context_json), request.schema_version);
      !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step != StepResult::row) {
    co_return std::unexpected(core::Error::storage("trace turn insert returned no row"));
  }
  auto record = read_turn_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = statement.expect_done("append_turn"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::move(*record);
}

async::Awaitable<core::Result<std::optional<TraceTurnRecord>>> TraceRepository::get_turn(TraceId turn_id) {
  if (core::is_zero_turn_id(turn_id)) {
    co_return std::unexpected(invalid_field("turn_id").with("reason", "zero_id"));
  }

  auto reader = co_await pool_->acquire_reader();
  if (!reader) {
    co_return std::unexpected(reader.error());
  }

  auto cached = reader->statement_cache().acquire(reader->connection(), kGetTurnSql);
  if (!cached) {
    co_return std::unexpected(cached.error());
  }
  auto& statement = cached->statement();
  if (auto bound = statement.bind_all(turn_id); !bound) {
    co_return std::unexpected(bound.error());
  }

  auto step = statement.step();
  if (!step) {
    co_return std::unexpected(step.error());
  }
  if (*step == StepResult::done) {
    co_return std::optional<TraceTurnRecord>{};
  }
  auto record = read_turn_row(statement);
  if (!record) {
    co_return std::unexpected(record.error());
  }
  if (auto done = statement.expect_done("get_turn"); !done) {
    co_return std::unexpected(done.error());
  }
  co_return std::optional<TraceTurnRecord>{std::move(*record)};
}

async::Awaitable<core::Result<std::vector<TraceTurnRecord>>>
TraceRepository::list_turns(ListTraceTurnsOptions options) {
  auto limit = checked_limit(options.limit);
  if (!limit) {
    co_return std::unexpected(limit.error());
  }
  if (options.session_id.has_value() && core::is_zero_turn_id(*options.session_id)) {
    co_return std::unexpected(invalid_field("session_id").with("reason", "zero_id"));
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
  if (options.session_id.has_value()) {
    if (auto bound = statement.bind_blob(index++, *options.session_id); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (!options.agent_key.empty()) {
    if (auto bound = statement.bind_text(index++, options.agent_key); !bound) {
      co_return std::unexpected(bound.error());
    }
  }
  if (auto bound = statement.bind_int64(index, *limit); !bound) {
    co_return std::unexpected(bound.error());
  }

  std::vector<TraceTurnRecord> rows;
  while (true) {
    auto step = statement.step();
    if (!step) {
      co_return std::unexpected(step.error());
    }
    if (*step == StepResult::done) {
      break;
    }
    auto record = read_turn_row(statement);
    if (!record) {
      co_return std::unexpected(record.error());
    }
    rows.push_back(std::move(*record));
  }

  co_return rows;
}

}  // namespace orangutan::storage
