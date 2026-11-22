#include <oran/bootstrap/runtime_assembly.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr std::string_view kAuditDatabaseRelative = ".orangutan/audit.db";
constexpr std::string_view kSessionsDatabaseRelative = ".orangutan/sessions.db";
constexpr std::string_view kMemoryDatabaseRelative = ".orangutan/memory.db";
template <typename T>
[[nodiscard]] Result<T>
run_result_inline(asio::io_context& io, async::Awaitable<Result<T>> operation, std::string_view operation_name) {
  auto completed = std::optional<Result<T>>{};
  auto failure = std::exception_ptr{};
  asio::co_spawn(io, std::move(operation), [&](std::exception_ptr error, Result<T> result) {
    failure = error;
    if (error == nullptr) {
      completed.emplace(std::move(result));
    }
  });
  io.run();

  if (failure != nullptr) {
    try {
      std::rethrow_exception(failure);
    } catch (const std::exception& error) {
      return std::unexpected(Error::internal("inline coroutine terminated by exception")
                                 .with("operation", std::string{operation_name})
                                 .with("detail", error.what()));
    } catch (...) {
      return std::unexpected(
          Error::internal("inline coroutine terminated by exception").with("operation", std::string{operation_name}));
    }
  }
  if (!completed.has_value()) {
    return std::unexpected(
        Error::internal("inline coroutine did not complete").with("operation", std::string{operation_name}));
  }
  return std::move(*completed);
}

[[nodiscard]] std::string
resolve_database_path(std::string_view workspace, std::string_view override_path, std::string_view relative_path) {
  if (!override_path.empty()) {
    return std::string{override_path};
  }
  auto path = std::filesystem::path{std::string{workspace}};
  path /= relative_path;
  return path.string();
}

[[nodiscard]] Result<void> ensure_parent_directory(const std::filesystem::path& target, std::string_view kind) {
  if (auto parent = target.parent_path(); !parent.empty()) {
    auto ec = std::error_code{};
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return std::unexpected(Error::io("failed to create runtime database directory")
                                 .with("path", parent.string())
                                 .with("database", std::string{kind})
                                 .with("detail", ec.message()));
    }
  }
  return {};
}

[[nodiscard]] Result<storage::MigrationReport> run_audit_migration_inline(const std::string& audit_path,
                                                                          std::size_t reader_count,
                                                                          std::size_t statement_cache_capacity) {
  asio::io_context io;
  auto temp_pool_result = storage::Pool::open(io.get_executor(),
                                              storage::PoolOptions{
                                                  .path = audit_path,
                                                  .reader_count = reader_count,
                                                  .statement_cache_capacity = statement_cache_capacity,
                                              });
  if (!temp_pool_result) {
    return std::unexpected(std::move(temp_pool_result).error());
  }
  auto temp_pool = std::move(*temp_pool_result);
  storage::AuditRepository repo{temp_pool};

  return run_result_inline(io, repo.migrate(), "audit migration");
}

/// Drive the session repository migration to completion before the long-lived
/// sessions pool is opened on the caller-supplied executor.
[[nodiscard]] Result<storage::MigrationReport> run_session_migration_inline(const std::string& sessions_path,
                                                                            std::size_t reader_count,
                                                                            std::size_t statement_cache_capacity) {
  asio::io_context io;
  auto temp_pool_result = storage::Pool::open(io.get_executor(),
                                              storage::PoolOptions{
                                                  .path = sessions_path,
                                                  .reader_count = reader_count,
                                                  .statement_cache_capacity = statement_cache_capacity,
                                              });
  if (!temp_pool_result) {
    return std::unexpected(std::move(temp_pool_result).error());
  }
  auto temp_pool = std::move(*temp_pool_result);
  storage::SessionRepository repo{temp_pool};

  return run_result_inline(io, repo.migrate(), "session migration");
}

/// Drive the long-term memory migration to completion before the long-lived
/// memory pool is opened on the caller-supplied executor.
[[nodiscard]] Result<storage::MigrationReport>
run_longterm_memory_migration_inline(const std::string& memory_path,
                                     std::size_t reader_count,
                                     std::size_t statement_cache_capacity) {
  asio::io_context io;
  auto temp_pool_result = storage::Pool::open(io.get_executor(),
                                              storage::PoolOptions{
                                                  .path = memory_path,
                                                  .reader_count = reader_count,
                                                  .statement_cache_capacity = statement_cache_capacity,
                                              });
  if (!temp_pool_result) {
    return std::unexpected(std::move(temp_pool_result).error());
  }
  auto temp_pool = std::move(*temp_pool_result);
  memory::longterm::Fts5Backend backend{temp_pool};

  return run_result_inline(io, backend.migrate(), "long-term memory migration");
}

}  // namespace

struct RuntimeAssembly::Impl {
  bool audit_enabled{false};
  std::string audit_path;
  std::string sessions_path;
  std::string longterm_memory_path;
  std::unique_ptr<storage::Pool> audit_pool;
  std::unique_ptr<storage::AuditRepository> audit_repository;
  std::unique_ptr<storage::TraceRepository> trace_repository;
  std::unique_ptr<storage::Pool> sessions_pool;
  std::unique_ptr<storage::SessionRepository> session_repository;
  std::unique_ptr<memory::session::Store> session_store;
  std::unique_ptr<storage::Pool> longterm_memory_pool;
  std::unique_ptr<memory::longterm::Fts5Backend> longterm_memory_backend;
  std::unique_ptr<permission::AuditSink> audit_sink;
  std::unique_ptr<permission::ApprovalBroker> approval_broker;
  std::unique_ptr<tool::Workspace> workspace;
  std::unique_ptr<hook::Bus> hook_bus;
};

RuntimeAssembly::RuntimeAssembly(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

RuntimeAssembly::RuntimeAssembly(RuntimeAssembly&&) noexcept = default;

RuntimeAssembly& RuntimeAssembly::operator=(RuntimeAssembly&&) noexcept = default;

RuntimeAssembly::~RuntimeAssembly() = default;

permission::AuditSink& RuntimeAssembly::audit_sink() noexcept {
  return *impl_->audit_sink;
}

permission::ApprovalBroker& RuntimeAssembly::approval_broker() noexcept {
  return *impl_->approval_broker;
}

bool RuntimeAssembly::audit_enabled() const noexcept {
  return impl_->audit_enabled;
}

std::string_view RuntimeAssembly::audit_path() const noexcept {
  return impl_->audit_path;
}

tool::Workspace& RuntimeAssembly::workspace() noexcept {
  return *impl_->workspace;
}

const tool::Workspace& RuntimeAssembly::workspace() const noexcept {
  return *impl_->workspace;
}

storage::TraceRepository* RuntimeAssembly::trace_repository() noexcept {
  return impl_->trace_repository.get();
}

bool RuntimeAssembly::trace_enabled() const noexcept {
  return impl_->trace_repository != nullptr;
}

hook::Bus& RuntimeAssembly::hook_bus() noexcept {
  return *impl_->hook_bus;
}

const hook::Bus& RuntimeAssembly::hook_bus() const noexcept {
  return *impl_->hook_bus;
}

memory::session::Store* RuntimeAssembly::session_store() noexcept {
  return impl_->session_store.get();
}

bool RuntimeAssembly::session_memory_enabled() const noexcept {
  return impl_->session_store != nullptr;
}

std::string_view RuntimeAssembly::sessions_path() const noexcept {
  return impl_->sessions_path;
}

memory::longterm::Backend* RuntimeAssembly::longterm_memory_backend() noexcept {
  return impl_->longterm_memory_backend.get();
}

bool RuntimeAssembly::longterm_memory_enabled() const noexcept {
  return impl_->longterm_memory_backend != nullptr;
}

std::string_view RuntimeAssembly::longterm_memory_path() const noexcept {
  return impl_->longterm_memory_path;
}

Result<RuntimeAssembly> RuntimeAssembly::build(std::string_view workspace,
                                               asio::any_io_executor blocking_executor,
                                               RuntimeAssemblyOptions options) {
  if (workspace.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path is empty"));
  }

  auto impl = std::make_unique<Impl>();
  impl->audit_enabled = options.audit_enabled;

  auto workspace_result = tool::Workspace::create(workspace, std::move(options.workspace_options));
  if (!workspace_result) {
    return std::unexpected(std::move(workspace_result).error());
  }
  impl->workspace = std::make_unique<tool::Workspace>(std::move(*workspace_result));

  auto broker_result = permission::ApprovalBroker::with_random_secret();
  if (!broker_result) {
    return std::unexpected(std::move(broker_result).error());
  }
  impl->approval_broker = std::make_unique<permission::ApprovalBroker>(std::move(*broker_result));
  impl->hook_bus = std::make_unique<hook::Bus>(hook::BusOptions{
      .blocking_timeout = options.hook_blocking_timeout,
      .advisory_timeout = options.hook_blocking_timeout,
  });
  if (options.session_memory_enabled) {
    impl->sessions_path = resolve_database_path(workspace, options.sessions_db_path, kSessionsDatabaseRelative);
    if (auto parent_ok = ensure_parent_directory(std::filesystem::path{impl->sessions_path}, "sessions"); !parent_ok) {
      return std::unexpected(std::move(parent_ok).error());
    }

    auto session_migration = run_session_migration_inline(impl->sessions_path,
                                                          options.session_reader_count,
                                                          options.session_statement_cache_capacity);
    if (!session_migration) {
      return std::unexpected(std::move(session_migration).error());
    }

    auto sessions_pool = storage::Pool::open(blocking_executor,
                                             storage::PoolOptions{
                                                 .path = impl->sessions_path,
                                                 .reader_count = options.session_reader_count,
                                                 .statement_cache_capacity = options.session_statement_cache_capacity,
                                             });
    if (!sessions_pool) {
      return std::unexpected(std::move(sessions_pool).error());
    }
    impl->sessions_pool = std::make_unique<storage::Pool>(std::move(*sessions_pool));
    impl->session_repository = std::make_unique<storage::SessionRepository>(*impl->sessions_pool);
    impl->session_store = std::make_unique<memory::session::Store>(*impl->session_repository);
  }

  if (options.longterm_memory_enabled) {
    impl->longterm_memory_path =
        resolve_database_path(workspace, options.longterm_memory_db_path, kMemoryDatabaseRelative);
    if (auto parent_ok = ensure_parent_directory(std::filesystem::path{impl->longterm_memory_path}, "memory");
        !parent_ok) {
      return std::unexpected(std::move(parent_ok).error());
    }

    auto longterm_migration = run_longterm_memory_migration_inline(impl->longterm_memory_path,
                                                                   options.longterm_memory_reader_count,
                                                                   options.longterm_memory_statement_cache_capacity);
    if (!longterm_migration) {
      return std::unexpected(std::move(longterm_migration).error());
    }

    auto memory_pool =
        storage::Pool::open(blocking_executor,
                            storage::PoolOptions{
                                .path = impl->longterm_memory_path,
                                .reader_count = options.longterm_memory_reader_count,
                                .statement_cache_capacity = options.longterm_memory_statement_cache_capacity,
                            });
    if (!memory_pool) {
      return std::unexpected(std::move(memory_pool).error());
    }
    impl->longterm_memory_pool = std::make_unique<storage::Pool>(std::move(*memory_pool));
    impl->longterm_memory_backend = std::make_unique<memory::longterm::Fts5Backend>(*impl->longterm_memory_pool);
  }

  if (!options.audit_enabled) {
    impl->audit_sink = std::make_unique<permission::NullAuditSink>();
    return RuntimeAssembly{std::move(impl)};
  }

  impl->audit_path = resolve_database_path(workspace, options.audit_db_path, kAuditDatabaseRelative);
  if (auto parent_ok = ensure_parent_directory(std::filesystem::path{impl->audit_path}, "audit"); !parent_ok) {
    return std::unexpected(std::move(parent_ok).error());
  }

  auto migration =
      run_audit_migration_inline(impl->audit_path, options.audit_reader_count, options.audit_statement_cache_capacity);
  if (!migration) {
    return std::unexpected(std::move(migration).error());
  }
  auto long_lived_pool = storage::Pool::open(blocking_executor,
                                             storage::PoolOptions{
                                                 .path = impl->audit_path,
                                                 .reader_count = options.audit_reader_count,
                                                 .statement_cache_capacity = options.audit_statement_cache_capacity,
                                             });
  if (!long_lived_pool) {
    return std::unexpected(std::move(long_lived_pool).error());
  }
  impl->audit_pool = std::make_unique<storage::Pool>(std::move(*long_lived_pool));
  impl->audit_repository = std::make_unique<storage::AuditRepository>(*impl->audit_pool);
  impl->audit_sink =
      std::make_unique<permission::StorageAuditSink>(*impl->audit_repository, std::move(blocking_executor));

  if (options.trace_enabled) {
    impl->trace_repository = std::make_unique<storage::TraceRepository>(*impl->audit_pool);
  }

  return RuntimeAssembly{std::move(impl)};
}

}  // namespace orangutan::bootstrap
