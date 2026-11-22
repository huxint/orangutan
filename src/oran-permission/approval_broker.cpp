// src/oran-permission/approval_broker.cpp — `ApprovalBroker` implementation.
//
// All cryptographic work (issue / verify / input_hash) is delegated to the
// owned `ApprovalAuthority`; the broker only owns the replay-window state.
// See the header for the operator-facing semantics.

#include <oran/permission/approval_broker.hpp>

#include <cstddef>
#include <cstring>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/permission/approval.hpp>

namespace orangutan::permission {

namespace {

[[nodiscard]] core::Error denied(std::string reason) {
  return core::Error::permission_denied("approval broker rejected check").with("reason", std::move(reason));
}

}  // namespace

std::size_t ApprovalBroker::KeyHash::operator()(const Key& key) const noexcept {
  // `input_hash` is the output of SHA-256, so any 8 contiguous bytes are
  // already uniformly distributed; the broker only needs to fold in tool +
  // identity so two grants that happen to share the first 64 bits of their
  // input hashes don't collide in the bucket.
  std::size_t prefix = 0;
  std::memcpy(&prefix, key.input_hash.data(), sizeof(prefix));
  const std::size_t tool_h = std::hash<std::string_view>{}(key.tool_name);
  const std::size_t identity_h = std::hash<std::string_view>{}(key.identity);
  // Mix with two distinct odd multipliers; the exact constants are not
  // load-bearing, only their non-zero-and-coprime-to-2 shape.
  return prefix ^ (tool_h * 0x9e3779b97f4a7c15ULL) ^ (identity_h * 0xc6bc279692b5c323ULL);
}

core::Result<ApprovalBroker> ApprovalBroker::with_random_secret() {
  auto authority = ApprovalAuthority::with_random_secret();
  if (!authority) {
    return std::unexpected(std::move(authority.error()));
  }
  return ApprovalBroker{std::move(*authority)};
}

ApprovalBroker::ApprovalBroker(ApprovalAuthority authority) noexcept : authority_(std::move(authority)) {}

std::size_t ApprovalBroker::reap_expired(core::Time now) {
  const auto before = grants_.size();
  std::erase_if(grants_, [now](const auto& kv) { return kv.second.expires_at <= now; });
  return before - grants_.size();
}

void ApprovalBroker::enforce_identity_ceiling(std::string_view identity, const Key& new_key) {
  std::size_t live_for_identity = 0;
  auto oldest = grants_.end();
  for (auto it = grants_.begin(); it != grants_.end(); ++it) {
    if (it->first.identity != identity || it->first == new_key) {
      continue;
    }
    ++live_for_identity;
    if (oldest == grants_.end() || it->second.sequence < oldest->second.sequence) {
      oldest = it;
    }
  }

  if (live_for_identity >= max_grants_per_identity && oldest != grants_.end()) {
    grants_.erase(oldest);
  }
}

ApprovalToken ApprovalBroker::approve(const ApprovalGrant& grant, core::Time now) {
  const ApprovalRequest request{
      .tool_name = grant.tool_name,
      .input = grant.input,
      .identity = grant.identity,
      .ttl = grant.ttl,
  };
  auto token = authority_.issue(request, now);

  Key key{
      .tool_name = token.tool_name,
      .identity = token.identity,
      .input_hash = token.input_hash,
  };
  reap_expired(now);
  enforce_identity_ceiling(token.identity, key);
  // `operator[]` overwrites: re-approving the same triple resets the
  // counter and bumps the expiry to match the new policy. The behavior is
  // documented on the header.
  grants_[std::move(key)] = Entry{
      .expires_at = token.expires_at,
      .remaining_uses = grant.replay_max,
      .sequence = next_sequence_++,
  };
  return token;
}

core::Result<void> ApprovalBroker::check(const ApprovalToken& token,
                                         std::string_view tool_name,
                                         std::string_view input,
                                         std::string_view identity,
                                         core::Time now) {
  // The authority owns expiry / tool / identity / input / MAC checks and
  // attaches its own `reason` context entries on failure (see
  // `approval.cpp`). Propagating the authority's error preserves those
  // entries verbatim, which the audit slice will rely on.
  if (auto verified = authority_.verify(token, tool_name, input, identity, now); !verified) {
    return std::unexpected(std::move(verified.error()));
  }

  const Key probe{
      .tool_name = std::string{tool_name},
      .identity = std::string{identity},
      .input_hash = ApprovalAuthority::input_hash(input),
  };
  const auto it = grants_.find(probe);
  if (it == grants_.end()) {
    return std::unexpected(denied("no_grant"));
  }
  if (it->second.remaining_uses == 0) {
    return std::unexpected(denied("replay_exhausted"));
  }
  --it->second.remaining_uses;
  return {};
}

std::size_t ApprovalBroker::outstanding_grants() const noexcept {
  return grants_.size();
}

}  // namespace orangutan::permission
