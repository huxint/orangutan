// src/oran-tool/version_token.hpp — internal helper for spec 0011 v1
// version tokens (`v1:<sha256(canonical_path)>:<size>:<mtime_ns>`).
//
// Private to `oran-tool` translation units. Lives under `src/` (not
// `include/oran/tool/`) because the token shape is an implementation
// detail of the built-in catalog: agents treat the token as opaque, and
// future built-ins (the `FileModify` v2 surface, the `code.*` family)
// will go through the same helper rather than re-spelling the shape.

#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>
#include <oran/io/fingerprint.hpp>
#include <oran/permission/approval.hpp>

namespace orangutan::tool::detail {

/// Lowercase hex-string of the supplied bytes. Kept inline so the three
/// file built-ins (`FileRead`, `FileWrite`, `FileEdit`) all share the
/// same wire spelling without paying a TU dependency on a `version_token.cpp`.
[[nodiscard]] inline std::string hex_lower(std::span<const std::byte> bytes) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  out.resize(bytes.size() * 2U);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto v = static_cast<std::uint8_t>(bytes[i]);
    out[(i * 2U) + 0U] = kHex[(v >> 4U) & 0x0FU];
    out[(i * 2U) + 1U] = kHex[v & 0x0FU];
  }
  return out;
}

/// Build the opaque version token. The path-hash discrimination reuses
/// the existing libsodium SHA-256 via `permission::ApprovalAuthority::input_hash`
/// — `oran-tool` already depends on `oran-permission`, so no new package
/// edge is needed.
[[nodiscard]] inline std::string version_token(std::string_view canonical_path, const io::FileFingerprint& fp) {
  const auto path_hash = permission::ApprovalAuthority::input_hash(canonical_path);
  return std::format("v1:{}:{}:{}", hex_lower(path_hash), fp.size_bytes, fp.mtime_ns);
}

/// Commit-time freshness verifier for atomic truncate writes. The io commit
/// path invokes this with the current target's fingerprint in the same
/// critical section as the rename, so a change between the pre-write token
/// check and the commit aborts the replacement instead of clobbering the
/// updated file.
[[nodiscard]] inline std::move_only_function<core::Result<void>(const io::FileFingerprint&)>
expected_version_verifier(std::string canonical_path, std::string expected) {
  return [path = std::move(canonical_path),
          expected = std::move(expected)](const io::FileFingerprint& fp) -> core::Result<void> {
    const auto current_token = version_token(path, fp);
    if (current_token == expected) {
      return {};
    }
    return std::unexpected(core::Error{core::ErrorKind::conflict, "file changed during write"}
                               .with("path", path)
                               .with("reason", "stale_fingerprint")
                               .with("expected", expected)
                               .with("fingerprint", current_token));
  };
}

}  // namespace orangutan::tool::detail
