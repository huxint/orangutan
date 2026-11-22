// src/oran-storage/migration_assets.cpp — built-in (compile-time embedded) migrations.
//
// `AuditRepository::migrate` and `SessionRepository::migrate` used to walk
// up from `CWD` looking for `src/oran-storage/migrations/audit/` or
// `migrations/sessions/`. Anything running the binary outside the repo
// (operator commands, smoke tests, the runtime assembly built by
// `bootstrap::run`) failed at that lookup. This TU embeds the canonical
// SQL files at compile time via C++26 `#embed`, so the SQL is reachable
// from any CWD — the `.sql` files remain the canonical artefact
// developers edit, and the compiler keeps the binary in sync with them.
//
// The `Migration` lists are returned as a `std::span` over a
// function-local `static const std::array`. The local-static idiom keeps
// the `std::string` initialization off the global-constructor critical
// path and is thread-safe to call from any caller (per [stmt.dcl]/4).
// `migrations_directory` overrides still win — the repositories check it
// before reaching for the built-ins.

#include <oran/storage/migrations.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace orangutan::storage {

namespace {

constexpr unsigned char kAuditInitialBytes[] = {
#embed "migrations/audit/0001-audit-initial.sql"
};

constexpr unsigned char kSessionsInitialBytes[] = {
#embed "migrations/sessions/0001-sessions-initial.sql"
};

constexpr unsigned char kSessionSkillActivationsBytes[] = {
#embed "migrations/sessions/0002-session-skill-activations.sql"
};

constexpr unsigned char kTraceInitialBytes[] = {
#embed "migrations/audit/0002-trace-turns-initial.sql"
};

constexpr unsigned char kAuditParentTurnIdBytes[] = {
#embed "migrations/audit/0003-audit-parent-turn-id.sql"
};

constexpr unsigned char kAuditEventKindBytes[] = {
#embed "migrations/audit/0004-audit-event-kind.sql"
};

constexpr unsigned char kAuditToolCallRollupsBytes[] = {
#embed "migrations/audit/0005-audit-tool-call-rollups.sql"
};

constexpr unsigned char kAuditDropToolCallRollupsBytes[] = {
#embed "migrations/audit/0006-audit-drop-tool-call-rollups.sql"
};

template <std::size_t N>
[[nodiscard]] std::string to_sql_string(const unsigned char (&bytes)[N]) {
  return std::string{reinterpret_cast<const char*>(bytes), N};
}

}  // namespace

std::span<const Migration> built_in_audit_migrations() {
  static const std::array<Migration, 6> kMigrations{
      Migration{
          .version = 1,
          .name = "audit-initial",
          .sql = to_sql_string(kAuditInitialBytes),
      },
      Migration{
          .version = 2,
          .name = "trace-turns-initial",
          .sql = to_sql_string(kTraceInitialBytes),
      },
      Migration{
          .version = 3,
          .name = "audit-parent-turn-id",
          .sql = to_sql_string(kAuditParentTurnIdBytes),
      },
      Migration{
          .version = 4,
          .name = "audit-event-kind",
          .sql = to_sql_string(kAuditEventKindBytes),
      },
      Migration{
          .version = 5,
          .name = "audit-tool-call-rollups",
          .sql = to_sql_string(kAuditToolCallRollupsBytes),
      },
      Migration{
          .version = 6,
          .name = "audit-drop-tool-call-rollups",
          .sql = to_sql_string(kAuditDropToolCallRollupsBytes),
      },
  };
  return kMigrations;
}

std::span<const Migration> built_in_session_migrations() {
  static const std::array<Migration, 2> kMigrations{
      Migration{
          .version = 1,
          .name = "sessions-initial",
          .sql = to_sql_string(kSessionsInitialBytes),
      },
      Migration{
          .version = 2,
          .name = "session-skill-activations",
          .sql = to_sql_string(kSessionSkillActivationsBytes),
      },
  };
  return kMigrations;
}

std::span<const Migration> built_in_trace_migrations() {
  return built_in_audit_migrations();
}

}  // namespace orangutan::storage
