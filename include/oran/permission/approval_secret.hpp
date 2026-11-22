#pragma once

#include <array>
#include <cstddef>
#include <span>

#include <oran/core/result.hpp>

namespace orangutan::permission {

class ApprovalSecret {
public:
  /// HMAC-SHA-256 key length in bytes. libsodium's
  /// `crypto_auth_hmacsha256_KEYBYTES` is `32`; mirrored here so callers
  /// can size buffers without pulling `<sodium.h>` through this header.
  static constexpr std::size_t key_bytes = 32;
  /// HMAC-SHA-256 output length in bytes (matches
  /// `crypto_auth_hmacsha256_BYTES`).
  static constexpr std::size_t mac_bytes = 32;

  /// Generate a fresh 32-byte key from libsodium's CSPRNG. Calls
  /// `sodium_init()` exactly once per process behind a `std::call_once`
  /// guard; subsequent calls reuse the initialized library. On the rare
  /// failure path (`sodium_init()` returns `-1` on an extremely broken
  /// system) the error is reported as `Error::internal` so callers can
  /// translate it cleanly via the project-wide `Result<T>` contract
  /// (rule C3).
  [[nodiscard]] static core::Result<ApprovalSecret> generate();

  /// Construct an `ApprovalSecret` from a caller-supplied key. The
  /// `bytes` span must be exactly `key_bytes` (32) bytes long; anything
  /// else returns `Error::invalid_argument` with the actual length
  /// attached as the `expected` / `actual` context entries. The key is
  /// copied into the object — the caller's buffer is *not* zeroed. The
  /// path exists for tests, RFC 4231 vector pinning, and the eventual
  /// "derive key from password" rotation flow described in
  /// `secrets-and-state.md`.
  [[nodiscard]] static core::Result<ApprovalSecret> from_bytes(std::span<const std::byte> bytes);

  /// Compute HMAC-SHA-256 over `message`. Empty messages are allowed and
  /// produce a well-defined MAC (RFC 2104 §2). The returned array is a
  /// fresh value type; no allocation, no shared state.
  [[nodiscard]] std::array<std::byte, mac_bytes> mac(std::span<const std::byte> message) const noexcept;

  /// Constant-time MAC equality check. Returns `false` immediately when
  /// the sizes differ (no information leak about the prefix). When the
  /// sizes match, delegates to libsodium's `sodium_memcmp`, which is
  /// documented constant-time over the buffer length. Use this rather
  /// than `==` on the raw arrays for any verification path that decides
  /// whether to honor an approval; the latter short-circuits and leaks
  /// timing on a mismatch.
  [[nodiscard]] static bool macs_equal(std::span<const std::byte> a, std::span<const std::byte> b) noexcept;

  ApprovalSecret(const ApprovalSecret&) = delete;
  ApprovalSecret& operator=(const ApprovalSecret&) = delete;
  ApprovalSecret(ApprovalSecret&&) noexcept;
  ApprovalSecret& operator=(ApprovalSecret&&) noexcept;
  ~ApprovalSecret();

private:
  ApprovalSecret() noexcept = default;

  std::array<std::byte, key_bytes> key_{};
};

}  // namespace orangutan::permission
