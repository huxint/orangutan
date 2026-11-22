#pragma once

#include <asio/any_io_executor.hpp>

#include <oran/permission/audit.hpp>

namespace orangutan::storage {
class AuditRepository;
}

namespace orangutan::permission {

/// Writes decisions on the supplied blocking executor, then resumes the caller
/// after storage completes. The caller keeps the borrowed repository alive
/// until every write finishes, including cancelled writes.
class StorageAuditSink final : public AuditSink {
public:
  StorageAuditSink(storage::AuditRepository& repository, asio::any_io_executor blocking_executor) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<void>> record(AuditEvent event) override;

private:
  storage::AuditRepository* repository_{};
  asio::any_io_executor blocking_executor_;
};

}  // namespace orangutan::permission
