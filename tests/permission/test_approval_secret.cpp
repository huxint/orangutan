// tests/permission/test_approval_secret.cpp — `ApprovalSecret` HMAC wrapper.
//
// `ApprovalSecret` is the per-process signing key the approval flow uses
// to detect tokens that were not issued by the current runtime. The tests
// pin three invariants:
//
//   1. Library invariants: 32-byte key/output, length-validated
//      `from_bytes` factory, generated keys are non-zero with high
//      probability.
//   2. HMAC correctness: HMAC-SHA-256 vectors round-trip against
//      OpenSSL-computed reference outputs (key derivation independent of
//      libsodium so the regression catches both wrapper *and* primitive
//      drift).
//   3. Verification semantics: same key + same message ⇒ identical MAC;
//      different keys ⇒ different MACs (this is the criterion-5
//      "fresh-secret invalidates prior approvals" property); constant-time
//      compare honors size mismatches as a negative result.
//
// Reference vectors were computed via:
//   echo -n "<msg>" | openssl dgst -sha256 -mac HMAC -macopt hexkey:<hex>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>
#include <oran/permission.hpp>

namespace perm = orangutan::permission;
using perm::ApprovalSecret;

namespace {

[[nodiscard]] std::array<std::byte, 32> iota_key_bytes() noexcept {
  std::array<std::byte, 32> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(i);
  }
  return out;
}

[[nodiscard]] std::array<std::byte, 32> zero_key_bytes() noexcept {
  return std::array<std::byte, 32>{};
}

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view sv) noexcept {
  return {reinterpret_cast<const std::byte*>(sv.data()), sv.size()};
}

[[nodiscard]] std::array<std::byte, 32> from_hex(std::string_view hex) {
  // 64 hex chars → 32 bytes. Tests only; not a hardened decoder.
  std::array<std::byte, 32> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    auto nibble = [](char c) noexcept -> std::uint8_t {
      if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
      }
      if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
      }
      return static_cast<std::uint8_t>(c - 'A' + 10);
    };
    const auto hi = nibble(hex[2 * i]);
    const auto lo = nibble(hex[2 * i + 1]);
    out[i] = static_cast<std::byte>((hi << 4) | lo);
  }
  return out;
}

}  // namespace

TEST_CASE("ApprovalSecret::generate produces a usable secret", "[unit][permission][approval_secret]") {
  auto a = ApprovalSecret::generate();
  REQUIRE(a.has_value());

  auto b = ApprovalSecret::generate();
  REQUIRE(b.has_value());

  // Two freshly-generated secrets should disagree on the same message
  // with overwhelming probability (probability of collision on 32-byte
  // CSPRNG keys is ~2^-256, so any sane CI passes without flakiness).
  const auto msg = as_bytes("ping");
  const auto mac_a = a->mac(msg);
  const auto mac_b = b->mac(msg);
  REQUIRE_FALSE(ApprovalSecret::macs_equal(mac_a, mac_b));
}

TEST_CASE("ApprovalSecret::from_bytes rejects wrong-length keys", "[unit][permission][approval_secret]") {
  const std::array<std::byte, 31> too_short{};
  const std::array<std::byte, 33> too_long{};

  auto r1 = ApprovalSecret::from_bytes(too_short);
  REQUIRE_FALSE(r1.has_value());
  REQUIRE(r1.error().kind() == orangutan::core::ErrorKind::invalid_argument);

  auto r2 = ApprovalSecret::from_bytes(too_long);
  REQUIRE_FALSE(r2.has_value());
  REQUIRE(r2.error().kind() == orangutan::core::ErrorKind::invalid_argument);
}

TEST_CASE("ApprovalSecret::mac matches RFC-style reference vectors", "[unit][permission][approval_secret]") {
  // Vector 1: key = 0..31, message = "Hi There". Computed offline via:
  //   `echo -n "Hi There" | openssl dgst -sha256 -mac HMAC -macopt
  //   hexkey:000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f`
  {
    const auto key = iota_key_bytes();
    auto secret = ApprovalSecret::from_bytes(key);
    REQUIRE(secret.has_value());
    const auto got = secret->mac(as_bytes("Hi There"));
    const auto expected = from_hex("278639ec02309d3afded1b273f1349ba63b9089c12476d716bee3ecc94673e9e");
    REQUIRE(ApprovalSecret::macs_equal(got, expected));
  }
  // Vector 2: key = all zeros, message = "". Computed via:
  //   `echo -n "" | openssl dgst -sha256 -mac HMAC -macopt
  //   hexkey:0000000000000000000000000000000000000000000000000000000000000000`
  {
    const auto key = zero_key_bytes();
    auto secret = ApprovalSecret::from_bytes(key);
    REQUIRE(secret.has_value());
    const auto got = secret->mac({});
    const auto expected = from_hex("b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    REQUIRE(ApprovalSecret::macs_equal(got, expected));
  }
  // Vector 3: key = 0..31, message = "orangutan approval mac". Computed via:
  //   `echo -n "orangutan approval mac" | openssl dgst -sha256 -mac HMAC
  //   -macopt hexkey:000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f`
  {
    const auto key = iota_key_bytes();
    auto secret = ApprovalSecret::from_bytes(key);
    REQUIRE(secret.has_value());
    const auto got = secret->mac(as_bytes("orangutan approval mac"));
    const auto expected = from_hex("f9844450d0216be4df26010c291a66c62de5bdf5f36b393af3427ceabcfea787");
    REQUIRE(ApprovalSecret::macs_equal(got, expected));
  }
}

TEST_CASE("ApprovalSecret::mac is deterministic for the same key + message", "[unit][permission][approval_secret]") {
  const auto key = iota_key_bytes();
  auto a = ApprovalSecret::from_bytes(key);
  auto b = ApprovalSecret::from_bytes(key);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  const auto msg = as_bytes("repeatable message");
  REQUIRE(ApprovalSecret::macs_equal(a->mac(msg), b->mac(msg)));
}

TEST_CASE("ApprovalSecret::mac diverges when only the message changes", "[unit][permission][approval_secret]") {
  const auto key = iota_key_bytes();
  auto secret = ApprovalSecret::from_bytes(key);
  REQUIRE(secret.has_value());

  const auto m1 = secret->mac(as_bytes("apple"));
  const auto m2 = secret->mac(as_bytes("apply"));  // 1-byte flip
  REQUIRE_FALSE(ApprovalSecret::macs_equal(m1, m2));
}

TEST_CASE("ApprovalSecret::macs_equal is size-mismatch safe", "[unit][permission][approval_secret]") {
  const std::array<std::byte, 32> a{};
  const std::array<std::byte, 31> b{};
  REQUIRE_FALSE(ApprovalSecret::macs_equal(a, b));

  const std::array<std::byte, 0> e1{};
  const std::array<std::byte, 0> e2{};
  REQUIRE(ApprovalSecret::macs_equal(e1, e2));
}

TEST_CASE("ApprovalSecret is move-only and re-keying produces a fresh signer", "[unit][permission][approval_secret]") {
  // Criterion-5 property in primitive form: a freshly generated secret
  // disagrees with the previous one on the same message, so any token
  // signed under the prior key fails MAC verification under the new key.
  auto first = ApprovalSecret::generate();
  REQUIRE(first.has_value());
  const auto msg = as_bytes("restart-replay-attempt");
  const auto mac_before = first->mac(msg);

  auto moved = std::move(*first);
  REQUIRE(ApprovalSecret::macs_equal(moved.mac(msg), mac_before));

  auto rotated = ApprovalSecret::generate();
  REQUIRE(rotated.has_value());
  REQUIRE_FALSE(ApprovalSecret::macs_equal(rotated->mac(msg), mac_before));
}
