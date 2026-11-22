#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <asio/any_io_executor.hpp>

#include <oran/core/result.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::hook {
class Bus;
}
namespace orangutan::memory::session {
class Store;
}
namespace orangutan::memory::longterm {
class Backend;
}  // namespace orangutan::memory::longterm
namespace orangutan::storage {
class TraceRepository;
}

namespace orangutan::bootstrap {

struct RuntimeAssemblyOptions {
  // Empty paths select the corresponding database under <workspace>/.orangutan.
  std::string audit_db_path{};
  bool audit_enabled{true};
  std::size_t audit_reader_count{1};
  std::size_t audit_statement_cache_capacity{4};
  tool::WorkspaceOptions workspace_options{};
  bool trace_enabled{true};
  std::chrono::milliseconds hook_blocking_timeout{2000};
  std::string sessions_db_path{};
  bool session_memory_enabled{true};
  std::size_t session_reader_count{2};
  std::size_t session_statement_cache_capacity{8};
  std::string longterm_memory_db_path{};
  bool longterm_memory_enabled{true};
  std::size_t longterm_memory_reader_count{2};
  std::size_t longterm_memory_statement_cache_capacity{16};
};

/// Owns runtime resources at stable addresses. Every session and dispatched
/// tool must finish before its borrowed assembly is destroyed.
class RuntimeAssembly {
public:
  /// Validate the workspace and migrate databases before exposing services.
  [[nodiscard]] static core::Result<RuntimeAssembly>
  build(std::string_view workspace, asio::any_io_executor blocking_executor, RuntimeAssemblyOptions options = {});

  ~RuntimeAssembly();
  RuntimeAssembly(const RuntimeAssembly&) = delete;
  RuntimeAssembly& operator=(const RuntimeAssembly&) = delete;
  RuntimeAssembly(RuntimeAssembly&&) noexcept;
  RuntimeAssembly& operator=(RuntimeAssembly&&) noexcept;

  [[nodiscard]] permission::AuditSink& audit_sink() noexcept;
  [[nodiscard]] permission::ApprovalBroker& approval_broker() noexcept;
  [[nodiscard]] bool audit_enabled() const noexcept;
  [[nodiscard]] std::string_view audit_path() const noexcept;
  [[nodiscard]] tool::Workspace& workspace() noexcept;
  [[nodiscard]] const tool::Workspace& workspace() const noexcept;
  [[nodiscard]] storage::TraceRepository* trace_repository() noexcept;
  [[nodiscard]] bool trace_enabled() const noexcept;
  [[nodiscard]] hook::Bus& hook_bus() noexcept;
  [[nodiscard]] const hook::Bus& hook_bus() const noexcept;

  // A disabled store returns nullptr and has no database path.
  [[nodiscard]] memory::session::Store* session_store() noexcept;
  [[nodiscard]] bool session_memory_enabled() const noexcept;
  [[nodiscard]] std::string_view sessions_path() const noexcept;
  [[nodiscard]] memory::longterm::Backend* longterm_memory_backend() noexcept;
  [[nodiscard]] bool longterm_memory_enabled() const noexcept;
  [[nodiscard]] std::string_view longterm_memory_path() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit RuntimeAssembly(std::unique_ptr<Impl> impl) noexcept;
};

}  // namespace orangutan::bootstrap
