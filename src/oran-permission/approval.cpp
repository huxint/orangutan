// src/oran-permission/approval.cpp — `ApprovalAuthority` implementation.
//
// libsodium is private to this TU (rule C6 in
// `docs/rules/critical-rules.md`); the header forward-declares nothing
// from `<sodium.h>` and exposes only the byte-array shapes the caller
// already sees from `ApprovalSecret`.

#include <oran/permission/approval.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sodium.h>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/permission/approval_secret.hpp>

namespace orangutan::permission {

namespace {

constexpr std::array<std::byte, 16> kDomainSeparator = []() consteval {
  constexpr std::string_view text{"oran-approval-v1"};
  static_assert(text.size() == 16, "domain separator must be exactly 16 bytes");
  std::array<std::byte, 16> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>(text[i]);
  }
  return bytes;
}();

constexpr std::byte kFormatVersion{0x01};

[[nodiscard]] unsigned char* byte_ptr(std::byte* p) noexcept {
  return reinterpret_cast<unsigned char*>(p);
}

void append_le_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
  }
}

void append_le_i64(std::vector<std::byte>& out, std::int64_t value) {
  const auto u = static_cast<std::uint64_t>(value);
  for (std::size_t i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::byte>((u >> (8 * i)) & 0xff));
  }
}

[[nodiscard]] std::int64_t to_unix_millis(core::Time t) noexcept {
  using namespace std::chrono;
  return duration_cast<milliseconds>(t.to_system_time_point().time_since_epoch()).count();
}

[[nodiscard]] core::Error denied(std::string reason) {
  return core::Error::permission_denied("approval token rejected").with("reason", std::move(reason));
}

void append_string_view(std::vector<std::byte>& out, std::string_view sv) {
  append_le_u32(out, static_cast<std::uint32_t>(sv.size()));
  const auto* p = reinterpret_cast<const std::byte*>(sv.data());
  out.insert(out.end(), p, p + sv.size());
}

}  // namespace

std::vector<std::byte> ApprovalAuthority::canonical_bytes(std::string_view tool_name,
                                                          std::string_view identity,
                                                          std::span<const std::byte, 32> input_hash,
                                                          std::span<const std::byte, 16> nonce,
                                                          core::Time expires_at) {
  // Length envelope: header (17) + 4 + |tool| + 4 + |identity| + 32 + 16 + 8.
  std::vector<std::byte> buf;
  buf.reserve(17 + 4 + tool_name.size() + 4 + identity.size() + 32 + 16 + 8);
  buf.insert(buf.end(), kDomainSeparator.begin(), kDomainSeparator.end());
  buf.push_back(kFormatVersion);
  append_string_view(buf, tool_name);
  append_string_view(buf, identity);
  buf.insert(buf.end(), input_hash.begin(), input_hash.end());
  buf.insert(buf.end(), nonce.begin(), nonce.end());
  append_le_i64(buf, to_unix_millis(expires_at));
  return buf;
}

std::array<std::byte, 32> ApprovalAuthority::input_hash(std::string_view input) noexcept {
  std::array<std::byte, 32> out{};
  static_assert(out.size() == crypto_hash_sha256_BYTES);
  ::crypto_hash_sha256(byte_ptr(out.data()),
                       input.empty() ? nullptr : reinterpret_cast<const unsigned char*>(input.data()),
                       static_cast<unsigned long long>(input.size()));
  return out;
}

core::Result<ApprovalAuthority> ApprovalAuthority::with_random_secret() {
  auto secret = ApprovalSecret::generate();
  if (!secret) {
    return std::unexpected(std::move(secret.error()));
  }
  return ApprovalAuthority{std::move(*secret)};
}

ApprovalAuthority::ApprovalAuthority(ApprovalSecret secret) noexcept : secret_(std::move(secret)) {}

ApprovalToken ApprovalAuthority::issue(const ApprovalRequest& req, core::Time now) const {
  ApprovalToken token;
  token.tool_name = std::string{req.tool_name};
  token.identity = std::string{req.identity};
  token.input_hash = input_hash(req.input);
  ::randombytes_buf(byte_ptr(token.nonce.data()), token.nonce.size());
  using namespace std::chrono;
  token.expires_at = core::Time{now.to_system_time_point() + req.ttl};
  const auto buf = canonical_bytes(token.tool_name, token.identity, token.input_hash, token.nonce, token.expires_at);
  token.mac = secret_.mac(buf);
  return token;
}

core::Result<void> ApprovalAuthority::verify(const ApprovalToken& token,
                                             std::string_view tool_name,
                                             std::string_view input,
                                             std::string_view identity,
                                             core::Time now) const {
  if (now >= token.expires_at) {
    return std::unexpected(denied("expired"));
  }
  if (token.tool_name != tool_name) {
    return std::unexpected(denied("tool_mismatch"));
  }
  if (token.identity != identity) {
    return std::unexpected(denied("identity_mismatch"));
  }
  const auto recomputed_input_hash = input_hash(input);
  if (!ApprovalSecret::macs_equal(recomputed_input_hash, token.input_hash)) {
    return std::unexpected(denied("input_mismatch"));
  }
  const auto buf = canonical_bytes(token.tool_name, token.identity, token.input_hash, token.nonce, token.expires_at);
  const auto recomputed_mac = secret_.mac(buf);
  if (!ApprovalSecret::macs_equal(recomputed_mac, token.mac)) {
    return std::unexpected(denied("mac_mismatch"));
  }
  return {};
}

}  // namespace orangutan::permission
