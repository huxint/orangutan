// src/oran-permission/approval_secret.cpp — libsodium-backed HMAC-SHA-256
// key wrapper.
//
// libsodium is private to this TU (rule C6 in
// `docs/rules/critical-rules.md`). The public header forward-declares
// nothing from libsodium; all libsodium types and functions are reached
// through `<sodium.h>` here only.

#include <oran/permission/approval_secret.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include <sodium.h>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>

namespace orangutan::permission {

namespace {

[[nodiscard]] core::Result<void> ensure_sodium_initialized() {
  // `sodium_init()` is documented thread-safe and idempotent in
  // libsodium 1.0.13+, but the documented contract still asks callers
  // to serialize the *first* call themselves. A `std::once_flag` keeps
  // us correct on every supported version and costs one atomic load
  // after the first successful init.
  static std::once_flag init_flag;
  static int init_result = 0;
  std::call_once(init_flag, [&] { init_result = ::sodium_init(); });
  if (init_result < 0) {
    return std::unexpected(core::Error::internal("sodium_init() failed"));
  }
  return {};
}

[[nodiscard]] unsigned char* byte_ptr(std::byte* p) noexcept {
  return reinterpret_cast<unsigned char*>(p);
}
[[nodiscard]] const unsigned char* byte_ptr(const std::byte* p) noexcept {
  return reinterpret_cast<const unsigned char*>(p);
}

}  // namespace

core::Result<ApprovalSecret> ApprovalSecret::generate() {
  if (auto r = ensure_sodium_initialized(); !r) {
    return std::unexpected(std::move(r.error()));
  }
  // libsodium's `randombytes_buf` reads from /dev/urandom on Linux and
  // matches the OS entropy contract on supported platforms. It cannot
  // fail in a way that surfaces a return code; if it did, the system
  // is unrecoverable.
  ApprovalSecret out;
  ::randombytes_buf(byte_ptr(out.key_.data()), out.key_.size());
  static_assert(ApprovalSecret::key_bytes == crypto_auth_hmacsha256_KEYBYTES);
  static_assert(ApprovalSecret::mac_bytes == crypto_auth_hmacsha256_BYTES);
  return out;
}

core::Result<ApprovalSecret> ApprovalSecret::from_bytes(std::span<const std::byte> bytes) {
  if (bytes.size() != key_bytes) {
    return std::unexpected(core::Error::invalid_argument("ApprovalSecret key must be exactly 32 bytes")
                               .with("expected", std::to_string(key_bytes))
                               .with("actual", std::to_string(bytes.size())));
  }
  ApprovalSecret out;
  std::memcpy(out.key_.data(), bytes.data(), key_bytes);
  return out;
}

std::array<std::byte, ApprovalSecret::mac_bytes>
ApprovalSecret::mac(std::span<const std::byte> message) const noexcept {
  std::array<std::byte, mac_bytes> out{};
  ::crypto_auth_hmacsha256(byte_ptr(out.data()),
                           message.empty() ? nullptr : byte_ptr(message.data()),
                           static_cast<unsigned long long>(message.size()),
                           byte_ptr(key_.data()));
  return out;
}

bool ApprovalSecret::macs_equal(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
  // Size-mismatch short-circuit does not leak the prefix — both buffers
  // are caller-supplied and observable. The constant-time guarantee
  // matters only after the sizes are known to match.
  if (a.size() != b.size()) {
    return false;
  }
  if (a.empty()) {
    return true;
  }
  return ::sodium_memcmp(byte_ptr(a.data()), byte_ptr(b.data()), a.size()) == 0;
}

ApprovalSecret::ApprovalSecret(ApprovalSecret&& other) noexcept : key_(other.key_) {
  ::sodium_memzero(byte_ptr(other.key_.data()), other.key_.size());
}

ApprovalSecret& ApprovalSecret::operator=(ApprovalSecret&& other) noexcept {
  if (this != &other) {
    ::sodium_memzero(byte_ptr(key_.data()), key_.size());
    key_ = other.key_;
    ::sodium_memzero(byte_ptr(other.key_.data()), other.key_.size());
  }
  return *this;
}

ApprovalSecret::~ApprovalSecret() {
  ::sodium_memzero(byte_ptr(key_.data()), key_.size());
}

}  // namespace orangutan::permission
