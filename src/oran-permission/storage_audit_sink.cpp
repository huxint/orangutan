#include <oran/permission/storage_audit_sink.hpp>

#include <exception>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/audit.hpp>
#include <oran/storage/audit_repository.hpp>

namespace orangutan::permission {

namespace {

async::Awaitable<core::Result<void>> write_audit(asio::any_io_executor executor,
                                                 async::Awaitable<core::Result<storage::AuditEventRecord>> operation) {
  if (!executor) {
    co_return std::unexpected(core::Error::invalid_argument("audit blocking executor is not configured"));
  }
  try {
    auto written = co_await asio::co_spawn(executor, std::move(operation), asio::use_awaitable);
    if (!written) {
      co_return std::unexpected(std::move(written).error());
    }
    const auto cancellation = co_await asio::this_coro::cancellation_state;
    if (cancellation.cancelled() != asio::cancellation_type::none) {
      co_return std::unexpected(core::Error::cancelled());
    }
    co_return core::Result<void>{};
  } catch (const std::system_error& error) {
    if (error.code() == asio::error::operation_aborted) {
      co_return std::unexpected(core::Error::cancelled());
    }
    co_return std::unexpected(core::Error::storage("audit write failed").with("cause", error.what()));
  } catch (const std::exception& error) {
    co_return std::unexpected(core::Error::storage("audit write failed").with("cause", error.what()));
  }
}

}  // namespace

StorageAuditSink::StorageAuditSink(storage::AuditRepository& repository,
                                   asio::any_io_executor blocking_executor) noexcept
    : repository_{&repository}, blocking_executor_{std::move(blocking_executor)} {}

async::Awaitable<core::Result<void>> StorageAuditSink::record(AuditEvent event) {
  storage::AppendAuditEventRequest request{
      .event_kind = std::move(event.event_kind),
      .scope_key = std::move(event.scope_key),
      .agent_key = std::move(event.agent_key),
      .tool_name = std::move(event.tool_name),
      .identity = std::move(event.identity),
      .verdict = std::string{core::enum_name(event.verdict)},
      .outcome = std::string{core::enum_name(event.outcome)},
      .reason = std::move(event.reason),
      .parent_turn_id = event.parent_turn_id,
      .metadata_json = std::move(event.metadata_json),
  };
  if (event.input_hash.has_value()) {
    request.input_hash_hex = to_hex(*event.input_hash);
  }

  co_return co_await write_audit(blocking_executor_, repository_->append_event(std::move(request)));
}

}  // namespace orangutan::permission
