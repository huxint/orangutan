#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>
#include <oran/tool/workspace.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace memory = orangutan::memory;
namespace permission = orangutan::permission;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;
namespace tool = orangutan::tool;

namespace {

using namespace std::chrono_literals;

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

/// RAII chdir guard. The built-in-migration path must not depend on CWD —
/// the slice-15 packaging swap pulled the SQL into the binary precisely
/// so a process launched from anywhere can provision audit.db. This guard
/// gives the test below a way to assert that property without leaking the
/// CWD change into sibling cases.
class CwdGuard {
public:
  explicit CwdGuard(const std::filesystem::path& destination) : original_(std::filesystem::current_path()) {
    std::filesystem::current_path(destination);
  }

  ~CwdGuard() {
    std::error_code ec;
    std::filesystem::current_path(original_, ec);
  }

  CwdGuard(const CwdGuard&) = delete;
  CwdGuard& operator=(const CwdGuard&) = delete;

private:
  std::filesystem::path original_;
};

permission::AuditEvent make_event(std::string scope, std::string tool, permission::AuditOutcome outcome) {
  permission::AuditEvent event;
  event.scope_key = std::move(scope);
  event.agent_key = "coder";
  event.tool_name = std::move(tool);
  event.identity = "operator-1";
  event.verdict = permission::Verdict::allow;
  event.outcome = outcome;
  event.reason = "test rule";
  return event;
}

[[nodiscard]] core::Time fixed_now() noexcept {
  using namespace std::chrono;
  return core::Time{sys_days{year{2026} / January / day{1}}};
}

memory::longterm::Record make_longterm_record() {
  const auto created = core::Time{core::Time::time_point{1s}};
  const auto updated = core::Time{core::Time::time_point{2s}};
  return memory::longterm::Record{
      .key = memory::longterm::RecordKey{.id = "lt-1", .scope_key = "cli"},
      .kind = memory::longterm::RecordKind::project,
      .title = "Runtime assembly memory",
      .body = "Long-term memory is owned by RuntimeAssembly.",
      .created_at = created,
      .updated_at = updated,
      .last_read_at = updated,
      .importance = 0.5,
      .tags = {"bootstrap", "memory"},
      .linked_record_ids = {},
  };
}

bool table_exists(const std::filesystem::path& db_path, std::string_view table) {
  auto connection = storage::Connection::open(storage::ConnectionOptions{
      .path = db_path.string(),
      .mode = storage::OpenMode::read_only,
      .enable_wal = false,
      .enforce_foreign_keys = false,
  });
  REQUIRE(connection.has_value());

  auto query = connection->prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
  REQUIRE(query.has_value());
  REQUIRE(query->bind_text(1, table).has_value());
  auto row = query->step();
  REQUIRE(row.has_value());
  return *row == storage::StepResult::row;
}

}  // namespace

TEST_CASE("RuntimeAssembly::build installs NullAuditSink when audit disabled", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-null"};
  asio::io_context io;

  auto assembly = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                    io.get_executor(),
                                                    bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
  REQUIRE(assembly.has_value());
  REQUIRE_FALSE(assembly->audit_enabled());
  REQUIRE(assembly->audit_path().empty());
  REQUIRE_FALSE(std::filesystem::exists(temp.path() / ".orangutan" / "audit.db"));
}

TEST_CASE("RuntimeAssembly::build installs a hook bus with blocking timeout",
          "[unit][bootstrap][runtime_assembly][hook]") {
  TempDir temp{"oran-assembly-hook-bus"};
  asio::io_context io;

  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = false;
  options.hook_blocking_timeout = 75ms;
  auto assembly = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor(), std::move(options));

  REQUIRE(assembly.has_value());
  auto& bus = assembly->hook_bus();
  bus.subscribe(
      hook::Sink{.id = "slow",
                 .decide = [](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<hook::HookDecision>> {
                   auto slept = co_await async::sleep_for(co_await asio::this_coro::executor, 1s);
                   if (!slept) {
                     co_return std::unexpected(std::move(slept).error());
                   }
                   co_return hook::HookDecision{};
                 }},
      {hook::Event::tool_before});
  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        auto decision = co_await bus.publish_blocking<hook::Event::tool_before>(hook::ToolBeforePayload{});
        REQUIRE(decision.has_value());
        REQUIRE(decision->reason == "hook_timeout");
        REQUIRE(decision->trace.at(0).elapsed == 75ms);
      },
      500ms);
}

TEST_CASE("RuntimeAssembly::build provisions audit.db at the workspace default path",
          "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-default-path"};
  asio::io_context io;

  auto assembly = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor());
  REQUIRE(assembly.has_value());
  REQUIRE(assembly->audit_enabled());
  REQUIRE(assembly->audit_path() == (temp.path() / ".orangutan" / "audit.db").string());
  const auto audit_db = temp.path() / ".orangutan" / "audit.db";
  REQUIRE(std::filesystem::exists(audit_db));
  REQUIRE(table_exists(audit_db, "audit_events"));
  REQUIRE(table_exists(audit_db, "trace_turns"));
}

TEST_CASE("RuntimeAssembly::build provisions sessions.db at the workspace default path",
          "[unit][bootstrap][runtime_assembly][memory]") {
  TempDir temp{"oran-assembly-sessions-default"};

  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor());
    REQUIRE(built.has_value());
    REQUIRE(built->session_memory_enabled());
    REQUIRE(built->session_store() != nullptr);
    REQUIRE(built->sessions_path() == (temp.path() / ".orangutan" / "sessions.db").string());

    const auto sessions_db = temp.path() / ".orangutan" / "sessions.db";
    REQUIRE(std::filesystem::exists(sessions_db));
    REQUIRE(table_exists(sessions_db, "sessions"));
    REQUIRE(table_exists(sessions_db, "session_messages"));
    REQUIRE(table_exists(sessions_db, "session_skill_activations"));

    auto appended = co_await built->session_store()->append(memory::session::SessionId{.value = "s-1"},
                                                            memory::session::AgentKey{.value = "coder"},
                                                            core::Message::user_text("hello"));
    REQUIRE(appended.has_value());
    auto loaded = co_await built->session_store()->load(memory::session::SessionId{.value = "s-1"},
                                                        memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 1);
    REQUIRE((*loaded)[0].blocks == core::Message::user_text("hello").blocks);
  });
}

TEST_CASE("RuntimeAssembly::build provisions memory.db at the workspace default path",
          "[unit][bootstrap][runtime_assembly][memory]") {
  TempDir temp{"oran-assembly-longterm-default"};

  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor());
    REQUIRE(built.has_value());
    REQUIRE(built->longterm_memory_enabled());
    REQUIRE(built->longterm_memory_backend() != nullptr);
    REQUIRE(built->longterm_memory_path() == (temp.path() / ".orangutan" / "memory.db").string());

    const auto memory_db = temp.path() / ".orangutan" / "memory.db";
    REQUIRE(std::filesystem::exists(memory_db));
    REQUIRE(table_exists(memory_db, "longterm_records"));
    REQUIRE(table_exists(memory_db, "longterm_records_fts"));

    auto record = make_longterm_record();
    auto upserted = co_await built->longterm_memory_backend()->upsert(record);
    REQUIRE(upserted.has_value());

    auto recalled = co_await memory::longterm::recall(*built->longterm_memory_backend(), memory::longterm::RecallRequest{
        .query =
            memory::longterm::Query{
                .scope_key = "cli",
                .text = "assembly",
                .kinds = {memory::longterm::RecordKind::project},
            },
        .limit = 5,
    });
    REQUIRE(recalled.has_value());
    REQUIRE(recalled->hits.size() == 1);
    REQUIRE(recalled->hits[0].record.key.id == "lt-1");
    REQUIRE(recalled->text.contains("Runtime assembly memory"));
  });
}

TEST_CASE("RuntimeAssembly::build honors an explicit audit DB path", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-explicit-path"};
  const auto explicit_path = (temp.path() / "nested" / "audit.db").string();
  asio::io_context io;

  auto assembly = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                    io.get_executor(),
                                                    bootstrap::RuntimeAssemblyOptions{.audit_db_path = explicit_path});
  REQUIRE(assembly.has_value());
  REQUIRE(assembly->audit_path() == explicit_path);
  REQUIRE(std::filesystem::exists(explicit_path));
}

TEST_CASE("RuntimeAssembly::build honors an explicit sessions DB path", "[unit][bootstrap][runtime_assembly][memory]") {
  TempDir temp{"oran-assembly-explicit-session-path"};
  const auto explicit_path = (temp.path() / "nested" / "sessions.db").string();
  asio::io_context io;

  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.sessions_db_path = explicit_path;
  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor(), std::move(options));

  REQUIRE(built.has_value());
  REQUIRE(built->session_memory_enabled());
  REQUIRE(built->sessions_path() == explicit_path);
  REQUIRE(std::filesystem::exists(explicit_path));
  REQUIRE(table_exists(explicit_path, "sessions"));
  REQUIRE(table_exists(explicit_path, "session_skill_activations"));
}

TEST_CASE("RuntimeAssembly::build honors an explicit long-term memory DB path",
          "[unit][bootstrap][runtime_assembly][memory]") {
  TempDir temp{"oran-assembly-explicit-memory-path"};
  const auto explicit_path = (temp.path() / "nested" / "memory.db").string();
  asio::io_context io;

  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.longterm_memory_db_path = explicit_path;
  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor(), std::move(options));

  REQUIRE(built.has_value());
  REQUIRE(built->longterm_memory_enabled());
  REQUIRE(built->longterm_memory_path() == explicit_path);
  REQUIRE(std::filesystem::exists(explicit_path));
  REQUIRE(table_exists(explicit_path, "longterm_records"));
}

TEST_CASE("RuntimeAssembly::build can disable session memory", "[unit][bootstrap][runtime_assembly][memory]") {
  TempDir temp{"oran-assembly-sessions-disabled"};
  asio::io_context io;

  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.session_memory_enabled = false;
  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor(), std::move(options));

  REQUIRE(built.has_value());
  REQUIRE_FALSE(built->session_memory_enabled());
  REQUIRE(built->session_store() == nullptr);
  REQUIRE(built->sessions_path().empty());
  REQUIRE_FALSE(std::filesystem::exists(temp.path() / ".orangutan" / "sessions.db"));
}

TEST_CASE("RuntimeAssembly::build can disable long-term memory", "[unit][bootstrap][runtime_assembly][memory]") {
  TempDir temp{"oran-assembly-longterm-disabled"};
  asio::io_context io;

  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.longterm_memory_enabled = false;
  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor(), std::move(options));

  REQUIRE(built.has_value());
  REQUIRE_FALSE(built->longterm_memory_enabled());
  REQUIRE(built->longterm_memory_backend() == nullptr);
  REQUIRE(built->longterm_memory_path().empty());
  REQUIRE_FALSE(std::filesystem::exists(temp.path() / ".orangutan" / "memory.db"));
}

TEST_CASE("RuntimeAssembly::build is idempotent on re-run", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-idempotent"};
  asio::io_context io;

  auto first = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor());
  REQUIRE(first.has_value());

  asio::io_context io2;
  auto second = bootstrap::RuntimeAssembly::build(temp.path().string(), io2.get_executor());
  REQUIRE(second.has_value());
  REQUIRE(std::filesystem::exists(temp.path() / ".orangutan" / "audit.db"));
  REQUIRE(std::filesystem::exists(temp.path() / ".orangutan" / "sessions.db"));
  REQUIRE(std::filesystem::exists(temp.path() / ".orangutan" / "memory.db"));
}

TEST_CASE("RuntimeAssembly::build rejects an empty workspace", "[unit][bootstrap][runtime_assembly]") {
  asio::io_context io;
  auto assembly = bootstrap::RuntimeAssembly::build({}, io.get_executor());
  REQUIRE_FALSE(assembly.has_value());
  REQUIRE(assembly.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("RuntimeAssembly storage sink records events end-to-end", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-storage-record"};

  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor());
    REQUIRE(built.has_value());
    auto assembly = std::move(*built);

    auto recorded =
        co_await assembly.audit_sink().record(make_event("scope-A", "FileRead", permission::AuditOutcome::allow));
    REQUIRE(recorded.has_value());
  });
}

TEST_CASE("RuntimeAssembly null sink discards events silently", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-null-record"};

  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto built = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                   io.get_executor(),
                                                   bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
    REQUIRE(built.has_value());
    auto assembly = std::move(*built);

    auto recorded =
        co_await assembly.audit_sink().record(make_event("scope-A", "FileRead", permission::AuditOutcome::allow));
    REQUIRE(recorded.has_value());
  });
}

TEST_CASE("RuntimeAssembly approval broker round-trips a fresh token", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-broker"};
  asio::io_context io;

  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                 io.get_executor(),
                                                 bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
  REQUIRE(built.has_value());
  auto assembly = std::move(*built);

  const auto now = fixed_now();
  permission::ApprovalGrant grant{
      .tool_name = "FileWrite",
      .input = "/tmp/note.txt",
      .identity = "operator-1",
      .ttl = std::chrono::seconds{60},
      .replay_max = 2,
  };
  auto token = assembly.approval_broker().approve(grant, now);
  REQUIRE(token.tool_name == "FileWrite");

  auto check = assembly.approval_broker().check(token, "FileWrite", "/tmp/note.txt", "operator-1", now);
  REQUIRE(check.has_value());
}

TEST_CASE("RuntimeAssembly approval broker rejects a cross-tool token", "[unit][bootstrap][runtime_assembly]") {
  TempDir temp{"oran-assembly-broker-cross-tool"};
  asio::io_context io;

  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                 io.get_executor(),
                                                 bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
  REQUIRE(built.has_value());
  auto assembly = std::move(*built);

  const auto now = fixed_now();
  permission::ApprovalGrant grant{
      .tool_name = "FileWrite",
      .input = "/tmp/note.txt",
      .identity = "operator-1",
  };
  auto token = assembly.approval_broker().approve(grant, now);

  auto cross_tool = assembly.approval_broker().check(token, "FileDelete", "/tmp/note.txt", "operator-1", now);
  REQUIRE_FALSE(cross_tool.has_value());
  REQUIRE(cross_tool.error().kind() == core::ErrorKind::permission_denied);
}

TEST_CASE("RuntimeAssembly::build provisions audit.db from a non-source CWD", "[unit][bootstrap][runtime_assembly]") {
  TempDir workspace{"oran-assembly-cwd-workspace"};
  TempDir cwd{"oran-assembly-cwd-elsewhere"};
  CwdGuard guard{cwd.path()};
  asio::io_context io;

  auto built = bootstrap::RuntimeAssembly::build(workspace.path().string(), io.get_executor());
  REQUIRE(built.has_value());
  REQUIRE(built->audit_enabled());
  REQUIRE(std::filesystem::exists(workspace.path() / ".orangutan" / "audit.db"));
}

TEST_CASE("RuntimeAssembly::build canonicalises the workspace root", "[unit][bootstrap][runtime_assembly][workspace]") {
  TempDir temp{"oran-assembly-workspace-root"};
  asio::io_context io;

  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(),
                                                 io.get_executor(),
                                                 bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
  REQUIRE(built.has_value());
  auto canonical = std::filesystem::weakly_canonical(temp.path()).string();
  REQUIRE(built->workspace().root() == canonical);
  REQUIRE(built->workspace().extra_read_roots().empty());
  REQUIRE(built->workspace().extra_write_roots().empty());
}

TEST_CASE("RuntimeAssembly::build widens workspace roots from options",
          "[unit][bootstrap][runtime_assembly][workspace]") {
  TempDir workspace{"oran-assembly-workspace-overrides"};
  TempDir extra_read{"oran-assembly-extra-read"};
  TempDir extra_write{"oran-assembly-extra-write"};
  asio::io_context io;

  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = false;
  options.workspace_options = tool::WorkspaceOptions{
      .extra_read_roots = {extra_read.path().string()},
      .extra_write_roots = {extra_write.path().string()},
  };

  auto built = bootstrap::RuntimeAssembly::build(workspace.path().string(), io.get_executor(), std::move(options));
  REQUIRE(built.has_value());
  REQUIRE(built->workspace().extra_read_roots().size() == 1);
  REQUIRE(built->workspace().extra_read_roots()[0] == std::filesystem::weakly_canonical(extra_read.path()).string());
  REQUIRE(built->workspace().extra_write_roots().size() == 1);
  REQUIRE(built->workspace().extra_write_roots()[0] == std::filesystem::weakly_canonical(extra_write.path()).string());
}

TEST_CASE("RuntimeAssembly::build rejects a non-existent workspace root",
          "[unit][bootstrap][runtime_assembly][workspace]") {
  asio::io_context io;
  auto bogus = (std::filesystem::temp_directory_path() / "oran-assembly-does-not-exist-7263").string();
  auto built = bootstrap::RuntimeAssembly::build(bogus,
                                                 io.get_executor(),
                                                 bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().kind() == core::ErrorKind::not_found);
}

TEST_CASE("RuntimeAssembly::build rejects an extra root that does not exist",
          "[unit][bootstrap][runtime_assembly][workspace]") {
  TempDir workspace{"oran-assembly-bad-extra-root"};
  asio::io_context io;

  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = false;
  options.workspace_options = tool::WorkspaceOptions{
      .extra_read_roots = {(std::filesystem::temp_directory_path() / "oran-assembly-no-such-extra").string()},
  };

  auto built = bootstrap::RuntimeAssembly::build(workspace.path().string(), io.get_executor(), std::move(options));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().kind() == core::ErrorKind::not_found);
}

TEST_CASE("RuntimeAssembly::build defaults to a live TraceRepository when audit is enabled",
          "[unit][bootstrap][runtime_assembly][trace]") {
  TempDir temp{"oran-assembly-trace-default"};

  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor());
    REQUIRE(built.has_value());
    REQUIRE(built->trace_enabled());
    REQUIRE(built->trace_repository() != nullptr);

    storage::TraceId turn_id{};
    storage::TraceId session_id{};
    for (std::size_t i = 0; i < turn_id.size(); ++i) {
      turn_id[i] = static_cast<std::byte>(0x10 + i);
      session_id[i] = static_cast<std::byte>(0x80 + i);
    }
    auto appended = co_await built->trace_repository()->append_turn(storage::AppendTraceTurnRequest{
        .turn_id = turn_id,
        .session_id = session_id,
        .agent_key = "coder",
        .origin = "bootstrap",
        .route_profile = "fake-main",
        .route_model = "fake-model",
        .started_at_ns = 1'000,
        .finished_at_ns = 1'025,
        .stop_reason = "end_turn",
    });
    REQUIRE(appended.has_value());

    auto turns = co_await built->trace_repository()->list_turns({});
    REQUIRE(turns.has_value());
    REQUIRE(turns->size() == 1);
  });
}

TEST_CASE("RuntimeAssembly::build omits the TraceRepository when trace_enabled is false",
          "[unit][bootstrap][runtime_assembly][trace]") {
  TempDir temp{"oran-assembly-trace-off"};
  asio::io_context io;

  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.trace_enabled = false;
  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor(), std::move(options));
  REQUIRE(built.has_value());
  REQUIRE(built->audit_enabled());
  REQUIRE_FALSE(built->trace_enabled());
  REQUIRE(built->trace_repository() == nullptr);
}

TEST_CASE("RuntimeAssembly::build forces trace off when audit is disabled",
          "[unit][bootstrap][runtime_assembly][trace]") {
  TempDir temp{"oran-assembly-trace-no-audit"};
  asio::io_context io;

  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = false;
  options.trace_enabled = true;
  auto built = bootstrap::RuntimeAssembly::build(temp.path().string(), io.get_executor(), std::move(options));
  REQUIRE(built.has_value());
  REQUIRE_FALSE(built->audit_enabled());
  REQUIRE_FALSE(built->trace_enabled());
  REQUIRE(built->trace_repository() == nullptr);
}
