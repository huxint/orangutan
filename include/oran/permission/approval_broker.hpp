#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/permission/approval.hpp>

namespace orangutan::permission {

/// Policy + identity inputs for `ApprovalBroker::approve`. Mirrors
/// `ApprovalRequest` but adds the replay-window fields the broker owns.
/// The strings are view-only at the boundary; the broker copies what it
/// needs into its internal map and into the returned token.
struct ApprovalGrant {
  std::string_view tool_name;
  std::string_view input;
  std::string_view identity;
  /// Validity window applied to the issued token's `expires_at` and to
  /// the broker's stored grant entry. Default matches the
  /// "Approval Signing" design value (1h).
  std::chrono::seconds ttl{3600};
  /// Total number of times the broker will honor this grant. Each
  /// successful `check` decrements by one; once zero, further checks
  /// return `replay_exhausted` until the grant expires or the operator
  /// re-approves. Default mirrors the design-doc `replay_max=8`. Zero
  /// is permitted and yields a grant that no `check` will honor —
  /// useful for "issue but quarantine" diagnostic flows.
  std::uint32_t replay_max{8};
};

/// Stateful façade over `ApprovalAuthority`. Owns the authority; the
/// broker's lifetime is the runtime's lifetime, and a new process gets a
/// fresh authority (which by criterion 5 invalidates every prior token).
class ApprovalBroker {
public:
  /// Maximum number of live grant entries retained per identity. This is
  /// the spec-0012 bounded-state ceiling for approval grants; TTL remains
  /// the primary lifecycle policy, and this cap prevents a single long-lived
  /// identity from growing the broker map without bound between reap ticks.
  static constexpr std::size_t max_grants_per_identity = 64;

  /// Build a broker that owns a freshly-generated `ApprovalAuthority`.
  /// Same fallibility envelope as `ApprovalAuthority::with_random_secret`.
  [[nodiscard]] static core::Result<ApprovalBroker> with_random_secret();

  /// Build a broker from a pre-existing authority. Useful for tests that
  /// want to share an authority across two brokers, or for the eventual
  /// `oran-bootstrap` wiring that builds the authority earlier in the
  /// startup sequence.
  explicit ApprovalBroker(ApprovalAuthority authority) noexcept;

  /// Issue a token for `grant` and register its replay entry. Overwrites
  /// any existing entry for the same `(tool, identity, input_hash)`
  /// triple — re-approving resets the counter, mirroring how operators
  /// expect "approve again" to behave.
  [[nodiscard]] ApprovalToken approve(const ApprovalGrant& grant, core::Time now);

  /// Verify `token` and decrement the replay counter for its triple.
  /// Returns `void` on success and `Error::permission_denied` with one
  /// of the documented `reason` entries on failure.
  [[nodiscard]] core::Result<void> check(const ApprovalToken& token,
                                         std::string_view tool_name,
                                         std::string_view input,
                                         std::string_view identity,
                                         core::Time now);

  /// Drop every grant whose `expires_at <= now`. Returns the number of
  /// entries removed. Operators may call this from a periodic job; the
  /// broker is otherwise lazy about expiry (expired entries simply fail
  /// `check` via the underlying authority's expiry check, then linger
  /// until reaped or overwritten).
  std::size_t reap_expired(core::Time now);

  /// Number of grants currently tracked (includes not-yet-reaped expired
  /// entries). Mostly for tests + diagnostics; the agent loop never
  /// queries this.
  [[nodiscard]] std::size_t outstanding_grants() const noexcept;

  /// Read-only access to the underlying authority. Exposed so the audit
  /// slice (and tests) can compute `input_hash` without going through
  /// the full approve / check round-trip.
  [[nodiscard]] const ApprovalAuthority& authority() const noexcept {
    return authority_;
  }

  ApprovalBroker(const ApprovalBroker&) = delete;
  ApprovalBroker& operator=(const ApprovalBroker&) = delete;
  ApprovalBroker(ApprovalBroker&&) noexcept = default;
  ApprovalBroker& operator=(ApprovalBroker&&) noexcept = default;
  ~ApprovalBroker() = default;

private:
  struct Key {
    std::string tool_name;
    std::string identity;
    std::array<std::byte, 32> input_hash{};

    friend bool operator==(const Key&, const Key&) = default;
  };

  struct KeyHash {
    [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
  };

  struct Entry {
    core::Time expires_at{};
    std::uint32_t remaining_uses{0};
    std::uint64_t sequence{0};
  };

  void enforce_identity_ceiling(std::string_view identity, const Key& new_key);

  ApprovalAuthority authority_;
  std::unordered_map<Key, Entry, KeyHash> grants_;
  std::uint64_t next_sequence_{0};
};

}  // namespace orangutan::permission
