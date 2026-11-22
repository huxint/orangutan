#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/storage/migrations.hpp>

namespace orangutan::storage {

class Pool;

using TraceId = core::TurnId;

struct AppendTraceTurnRequest {
  TraceId turn_id{};
  std::optional<TraceId> parent_turn_id{};
  TraceId session_id{};
  std::string agent_key;
  std::string origin;
  std::string route_profile;
  std::string route_model;
  std::int64_t started_at_ns{};
  std::int64_t finished_at_ns{};
  std::string stop_reason;
  std::int64_t iteration_count{1};
  std::uint64_t prompt_prefix_hash{};
  std::int64_t prompt_prefix_bytes{};
  std::uint64_t active_catalog_hash{};
  std::int64_t cache_creation_tokens{};
  std::int64_t cache_read_tokens{};
  std::int64_t input_tokens{};
  std::int64_t output_tokens{};
  double cost_estimate_usd{};
  std::optional<std::string> cancellation_phase{};
  std::string context_json{"{}"};
  std::int64_t schema_version{1};
};

struct TraceTurnRecord {
  TraceId turn_id{};
  std::optional<TraceId> parent_turn_id{};
  TraceId session_id{};
  std::string agent_key;
  std::string origin;
  std::string route_profile;
  std::string route_model;
  std::int64_t started_at_ns{};
  std::int64_t finished_at_ns{};
  std::string stop_reason;
  std::int64_t iteration_count{};
  std::uint64_t prompt_prefix_hash{};
  std::int64_t prompt_prefix_bytes{};
  std::uint64_t active_catalog_hash{};
  std::int64_t cache_creation_tokens{};
  std::int64_t cache_read_tokens{};
  std::int64_t input_tokens{};
  std::int64_t output_tokens{};
  double cost_estimate_usd{};
  std::optional<std::string> cancellation_phase{};
  std::string context_json;
  std::int64_t schema_version{};
};

struct ListTraceTurnsOptions {
  std::optional<TraceId> session_id{};
  std::string agent_key{};
  std::size_t limit{50};
};

struct TraceRepositoryOptions {
  std::string migrations_directory;
};

class TraceRepository {
public:
  explicit TraceRepository(Pool& pool, TraceRepositoryOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MigrationReport>> migrate();

  [[nodiscard]] async::Awaitable<core::Result<TraceTurnRecord>> append_turn(AppendTraceTurnRequest request);

  [[nodiscard]] async::Awaitable<core::Result<std::optional<TraceTurnRecord>>> get_turn(TraceId turn_id);

  [[nodiscard]] async::Awaitable<core::Result<std::vector<TraceTurnRecord>>> list_turns(ListTraceTurnsOptions options);

private:
  Pool* pool_{};
  TraceRepositoryOptions options_;
};

}  // namespace orangutan::storage
