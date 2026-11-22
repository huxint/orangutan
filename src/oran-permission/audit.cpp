// src/oran-permission/audit.cpp — permission audit sink implementations.

#include <oran/permission/audit.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

namespace {

constexpr char nibble_to_hex(std::uint8_t nibble) noexcept {
  return static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

}  // namespace

async::Awaitable<core::Result<void>> NullAuditSink::record(AuditEvent /*event*/) {
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> RecordingAuditSink::record(AuditEvent event) {
  events_.push_back(std::move(event));
  co_return core::Result<void>{};
}

AuditEvent make_audit_event_from_decision(const Decision& decision) {
  AuditEvent event;
  event.verdict = decision.verdict;
  event.outcome = verdict_to_outcome(decision.verdict);
  event.reason = decision.reason;
  return event;
}

std::string to_hex(std::span<const std::byte, 32> input_hash) {
  std::string out;
  out.reserve(64);
  for (auto byte : input_hash) {
    const auto value = static_cast<std::uint8_t>(byte);
    out.push_back(nibble_to_hex(static_cast<std::uint8_t>(value >> 4)));
    out.push_back(nibble_to_hex(static_cast<std::uint8_t>(value & 0x0F)));
  }
  return out;
}

}  // namespace orangutan::permission
