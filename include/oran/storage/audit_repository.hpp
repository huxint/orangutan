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

/// Required text fields must be non-empty. The database assigns id and created_at.
struct AppendAuditEventRequest {
  std::string event_kind{"permission_decision"};
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;

  // Verdict and outcome retain the permission enums' wire spellings.
  std::string verdict;
  std::string outcome;
  std::string reason;

  // Empty hashes are stored as SQL NULL; supplied hashes use lowercase SHA-256 hex.
  std::string input_hash_hex{};
  std::optional<core::TurnId> parent_turn_id{};

  // Callers own JSON shape; the repository requires non-empty text.
  std::string metadata_json{"{}"};
};

struct AuditEventRecord {
  std::int64_t id{};
  /// Older databases may also hold `hook_publish` rows.
  std::string event_kind;
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;
  std::string verdict;
  std::string outcome;
  std::string reason;
  std::optional<std::string> input_hash_hex;
  std::optional<core::TurnId> parent_turn_id;
  std::string metadata_json;
  std::string created_at;
};

struct ListAuditEventsOptions {
  std::string scope_key;

  // Empty secondary filters match every value in the required scope.
  std::string agent_key{};
  std::string tool_name{};
  std::string event_kind{};
  std::string outcome{};
  std::size_t limit{50};
};

struct AuditRepositoryOptions {
  std::string migrations_directory;
};

/// Borrows the pool, which must outlive all repository operations.
class AuditRepository {
public:
  explicit AuditRepository(Pool& pool, AuditRepositoryOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MigrationReport>> migrate();

  [[nodiscard]] async::Awaitable<core::Result<AuditEventRecord>> append_event(AppendAuditEventRequest request);

  /// Returns scoped records in descending id order.
  [[nodiscard]] async::Awaitable<core::Result<std::vector<AuditEventRecord>>>
  list_events(ListAuditEventsOptions options);

private:
  Pool* pool_{};
  AuditRepositoryOptions options_;
};

}  // namespace orangutan::storage
