// tests/permission/test_audit.cpp — AuditEvent + AuditSink interface coverage.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;

namespace {

class TempDb {
public:
  explicit TempDb(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".db")) {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

permission::AuditEvent make_audit_event(std::string scope, std::string tool, permission::AuditOutcome outcome) {
  permission::AuditEvent event;
  event.scope_key = std::move(scope);
  event.agent_key = "coder";
  event.tool_name = std::move(tool);
  event.identity = "operator-1";
  event.verdict = permission::Verdict::allow;
  event.outcome = outcome;
  event.reason = "rule #1 (allow: File*)";
  return event;
}

core::TurnId turn_id_with(unsigned char seed) {
  core::TurnId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(seed + i);
  }
  return id;
}

}  // namespace

TEST_CASE("AuditOutcome wire spelling round-trips through core::enum_name/parse_enum", "[unit][permission][audit]") {
  using O = permission::AuditOutcome;
  for (auto outcome : core::enum_values<O>()) {
    const auto name = core::enum_name(outcome);
    REQUIRE_FALSE(name.empty());
    auto parsed = core::parse_enum<permission::AuditOutcome>(name);
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == outcome);
  }
  REQUIRE(core::enum_name(permission::AuditOutcome::approved) == "approved");
  REQUIRE(core::enum_name(permission::AuditOutcome::rejected) == "rejected");
  REQUIRE(core::enum_name(permission::AuditOutcome::blocked_by_hook) == "blocked_by_hook");
  REQUIRE(core::enum_name(permission::AuditOutcome::rewritten) == "rewritten");
}

TEST_CASE("verdict_to_outcome maps each Verdict to its rule-engine outcome", "[unit][permission][audit]") {
  REQUIRE(permission::verdict_to_outcome(permission::Verdict::allow) == permission::AuditOutcome::allow);
  REQUIRE(permission::verdict_to_outcome(permission::Verdict::deny) == permission::AuditOutcome::deny);
  REQUIRE(permission::verdict_to_outcome(permission::Verdict::ask) == permission::AuditOutcome::ask);
}

TEST_CASE("make_audit_event_from_decision copies verdict, outcome, reason", "[unit][permission][audit]") {
  permission::Decision decision;
  decision.verdict = permission::Verdict::ask;
  decision.reason = "rule #3 (ask: ShellExec)";
  decision.replay_max = 4;
  decision.approval_ttl = std::chrono::seconds{1800};

  auto event = permission::make_audit_event_from_decision(decision);
  REQUIRE(event.verdict == permission::Verdict::ask);
  REQUIRE(event.outcome == permission::AuditOutcome::ask);
  REQUIRE(event.reason == "rule #3 (ask: ShellExec)");
  REQUIRE(event.scope_key.empty());
  REQUIRE(event.metadata_json == "{}");
}

TEST_CASE("permission::to_hex matches RFC 6234 SHA-256 of the empty string", "[unit][permission][audit]") {
  // RFC 6234 §8.5 / NIST FIPS 180-4 — SHA-256("") =
  // e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  std::array<std::byte, 32> digest{};
  constexpr std::uint8_t bytes[32] = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
                                      0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
                                      0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
  for (std::size_t i = 0; i < digest.size(); ++i) {
    digest[i] = std::byte{bytes[i]};
  }
  REQUIRE(permission::to_hex(digest) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("NullAuditSink discards every event and returns success", "[unit][permission][audit]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    permission::NullAuditSink sink;
    auto r1 = co_await sink.record(make_audit_event("scope-A", "FileRead", permission::AuditOutcome::allow));
    REQUIRE(r1.has_value());
    auto r2 = co_await sink.record(make_audit_event("scope-A", "ShellExec", permission::AuditOutcome::approved));
    REQUIRE(r2.has_value());
  });
}

TEST_CASE("RecordingAuditSink captures events in insertion order", "[unit][permission][audit]") {
  test::run_async([](asio::io_context& /*io*/) -> async::Awaitable<void> {
    permission::RecordingAuditSink sink;
    REQUIRE(sink.events().empty());

    auto r1 = co_await sink.record(make_audit_event("scope-A", "FileRead", permission::AuditOutcome::allow));
    REQUIRE(r1.has_value());
    auto r2 = co_await sink.record(make_audit_event("scope-A", "FileWrite", permission::AuditOutcome::deny));
    REQUIRE(r2.has_value());

    auto events = sink.events();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].tool_name == "FileRead");
    REQUIRE(events[0].outcome == permission::AuditOutcome::allow);
    REQUIRE(events[1].tool_name == "FileWrite");
    REQUIRE(events[1].outcome == permission::AuditOutcome::deny);

    sink.clear();
    REQUIRE(sink.events().empty());
  });
}

TEST_CASE("StorageAuditSink persists events into the audit repository with correct columns",
          "[unit][permission][audit]") {
  TempDb db{"oran-permission-audit-sink"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool_result = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
    REQUIRE(pool_result.has_value());
    auto pool = std::move(*pool_result);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    permission::StorageAuditSink sink{repo, io.get_executor()};

    auto event = make_audit_event("scope-A", "FileRead", permission::AuditOutcome::allow);
    event.input_hash = std::array<std::byte, 32>{};
    for (std::size_t i = 0; i < 32; ++i) {
      (*event.input_hash)[i] = std::byte{0xAB};
    }
    event.parent_turn_id = turn_id_with(0x22);
    event.metadata_json = R"json({"trace_id":"abc"})json";

    auto recorded = co_await sink.record(std::move(event));
    REQUIRE(recorded.has_value());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    const auto& row = (*listed)[0];
    REQUIRE(row.event_kind == "permission_decision");
    REQUIRE(row.scope_key == "scope-A");
    REQUIRE(row.agent_key == "coder");
    REQUIRE(row.tool_name == "FileRead");
    REQUIRE(row.identity == "operator-1");
    REQUIRE(row.verdict == "allow");
    REQUIRE(row.outcome == "allow");
    REQUIRE(row.reason == "rule #1 (allow: File*)");
    REQUIRE(row.input_hash_hex.has_value());
    // 32 bytes of 0xAB hex-encode to "ab" repeated 32 times.
    REQUIRE(*row.input_hash_hex == "abababababababababababababababababababababababababababababababab");
    REQUIRE(row.parent_turn_id.has_value());
    REQUIRE(*row.parent_turn_id == turn_id_with(0x22));
    REQUIRE(row.metadata_json == R"json({"trace_id":"abc"})json");
  });
}

TEST_CASE("StorageAuditSink records a NULL input_hash when the event omits it", "[unit][permission][audit]") {
  TempDb db{"oran-permission-audit-sink-null-hash"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool_result = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
    REQUIRE(pool_result.has_value());
    auto pool = std::move(*pool_result);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    permission::StorageAuditSink sink{repo, io.get_executor()};

    auto event = make_audit_event("scope-A", "FileRead", permission::AuditOutcome::deny);
    event.verdict = permission::Verdict::deny;
    auto recorded = co_await sink.record(std::move(event));
    REQUIRE(recorded.has_value());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE_FALSE((*listed)[0].input_hash_hex.has_value());
    REQUIRE((*listed)[0].verdict == "deny");
    REQUIRE((*listed)[0].outcome == "deny");
  });
}

TEST_CASE("StorageAuditSink writes each AuditOutcome wire spelling", "[unit][permission][audit]") {
  TempDb db{"oran-permission-audit-sink-outcomes"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool_result = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
    REQUIRE(pool_result.has_value());
    auto pool = std::move(*pool_result);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    permission::StorageAuditSink sink{repo, io.get_executor()};

    using O = permission::AuditOutcome;
    constexpr auto outcomes = core::enum_values<O>();
    for (auto outcome : outcomes) {
      auto event = make_audit_event("scope-A", "FileRead", outcome);
      auto recorded = co_await sink.record(std::move(event));
      REQUIRE(recorded.has_value());
    }

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 100});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == outcomes.size());
    // list_events orders newest-first; reverse the expected vector.
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
      const auto expected = outcomes[outcomes.size() - 1 - i];
      REQUIRE((*listed)[i].outcome == core::enum_name(expected));
    }
  });
}

TEST_CASE("StorageAuditSink propagates repository errors", "[unit][permission][audit]") {
  TempDb db{"oran-permission-audit-sink-err"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool_result = storage::Pool::open(
        io.get_executor(),
        storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
    REQUIRE(pool_result.has_value());
    auto pool = std::move(*pool_result);
    storage::AuditRepository repo{pool};
    // No migrate() — the repository's append_event will fail because
    // audit_events table does not exist. The sink must surface that
    // error rather than swallow it.

    permission::StorageAuditSink sink{repo, io.get_executor()};
    auto recorded = co_await sink.record(make_audit_event("scope-A", "FileRead", permission::AuditOutcome::allow));
    REQUIRE_FALSE(recorded.has_value());
    REQUIRE(recorded.error().kind() == core::ErrorKind::storage);
  });
}

TEST_CASE("StorageAuditSink requires an explicit worker executor", "[unit][permission][audit][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    storage::Pool pool;
    storage::AuditRepository repository{pool};
    permission::StorageAuditSink sink{repository, {}};

    auto result = co_await sink.record(make_audit_event("scope-A", "FileRead", permission::AuditOutcome::allow));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind() == core::ErrorKind::invalid_argument);
    CHECK(result.error().message() == "audit blocking executor is not configured");
  });
}
