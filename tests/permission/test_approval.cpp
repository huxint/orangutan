#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/permission.hpp>

namespace perm = orangutan::permission;
namespace core = orangutan::core;
using core::ErrorKind;
using perm::ApprovalAuthority;
using perm::ApprovalRequest;
using perm::ApprovalSecret;
using perm::ApprovalToken;

namespace {

[[nodiscard]] ApprovalAuthority make_authority() {
  auto r = ApprovalAuthority::with_random_secret();
  REQUIRE(r.has_value());
  return std::move(*r);
}

[[nodiscard]] core::Time fixed_base() noexcept {
  // 2026-01-01T00:00:00Z — a stable instant that does not depend on the
  // wall clock and is comfortably away from epoch / year-9999 edges.
  using namespace std::chrono;
  const auto ymd = year{2026} / January / day{1};
  return core::Time{sys_days{ymd}};
}

[[nodiscard]] std::string reason_of(const core::Error& e) {
  for (const auto& [k, v] : e.context()) {
    if (k == "reason") {
      return v;
    }
  }
  return {};
}

}  // namespace

TEST_CASE("ApprovalAuthority round-trips matching args", "[unit][permission][approval]") {
  const auto authority = make_authority();
  const auto now = fixed_base();
  const auto token = authority.issue(
      ApprovalRequest{
          .tool_name = "ShellExec",
          .input = "ls /tmp",
          .identity = "operator",
          .ttl = std::chrono::seconds{60},
      },
      now);
  REQUIRE(token.tool_name == "ShellExec");
  REQUIRE(token.identity == "operator");

  // verify just after issue is well within the TTL.
  const auto verified = authority.verify(token, "ShellExec", "ls /tmp", "operator", now);
  REQUIRE(verified.has_value());
}

TEST_CASE("ApprovalAuthority rejects expired tokens", "[unit][permission][approval]") {
  const auto authority = make_authority();
  const auto now = fixed_base();
  const auto token = authority.issue(
      ApprovalRequest{
          .tool_name = "ShellExec",
          .input = "rm -rf /tmp/scratch",
          .identity = "operator",
          .ttl = std::chrono::seconds{30},
      },
      now);

  // Same instant as `expires_at` is treated as expired (>=).
  const auto at_expiry = core::Time{now.to_system_time_point() + std::chrono::seconds{30}};
  const auto r = authority.verify(token, "ShellExec", "rm -rf /tmp/scratch", "operator", at_expiry);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind() == ErrorKind::permission_denied);
  REQUIRE(reason_of(r.error()) == "expired");

  // 1 ms before expiry passes.
  const auto just_before = core::Time{now.to_system_time_point() + std::chrono::seconds{29}};
  const auto ok = authority.verify(token, "ShellExec", "rm -rf /tmp/scratch", "operator", just_before);
  REQUIRE(ok.has_value());
}

TEST_CASE("ApprovalAuthority rejects cross-tool replay", "[unit][permission][approval]") {
  const auto authority = make_authority();
  const auto now = fixed_base();
  const auto token =
      authority.issue(ApprovalRequest{.tool_name = "FileWrite", .input = "/etc/passwd", .identity = "operator"}, now);

  const auto r = authority.verify(token, "ShellExec", "/etc/passwd", "operator", now);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(reason_of(r.error()) == "tool_mismatch");
}

TEST_CASE("ApprovalAuthority rejects cross-input replay", "[unit][permission][approval]") {
  const auto authority = make_authority();
  const auto now = fixed_base();
  const auto token =
      authority.issue(ApprovalRequest{.tool_name = "FileWrite", .input = "/tmp/safe", .identity = "operator"}, now);

  const auto r = authority.verify(token, "FileWrite", "/tmp/danger", "operator", now);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(reason_of(r.error()) == "input_mismatch");
}

TEST_CASE("ApprovalAuthority rejects cross-identity replay", "[unit][permission][approval]") {
  const auto authority = make_authority();
  const auto now = fixed_base();
  const auto token =
      authority.issue(ApprovalRequest{.tool_name = "FileWrite", .input = "/tmp/file", .identity = "alice"}, now);

  const auto r = authority.verify(token, "FileWrite", "/tmp/file", "bob", now);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(reason_of(r.error()) == "identity_mismatch");
}

TEST_CASE("ApprovalAuthority detects MAC tampering", "[unit][permission][approval]") {
  const auto authority = make_authority();
  const auto now = fixed_base();
  auto token =
      authority.issue(ApprovalRequest{.tool_name = "MemoryWrite", .input = "fact-1", .identity = "operator"}, now);

  // Flip the last byte of the MAC.
  token.mac.back() = std::byte{static_cast<unsigned char>(std::to_integer<unsigned char>(token.mac.back()) ^ 0xff)};

  const auto r = authority.verify(token, "MemoryWrite", "fact-1", "operator", now);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(reason_of(r.error()) == "mac_mismatch");
}

TEST_CASE("ApprovalAuthority detects nonce tampering", "[unit][permission][approval]") {
  const auto authority = make_authority();
  const auto now = fixed_base();
  auto token =
      authority.issue(ApprovalRequest{.tool_name = "MemoryWrite", .input = "fact-1", .identity = "operator"}, now);

  // Mutate the nonce; everything else stays consistent with the original
  // canonical bytes, so the MAC must no longer line up.
  token.nonce.front() =
      std::byte{static_cast<unsigned char>(std::to_integer<unsigned char>(token.nonce.front()) ^ 0x55)};

  const auto r = authority.verify(token, "MemoryWrite", "fact-1", "operator", now);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(reason_of(r.error()) == "mac_mismatch");
}

TEST_CASE("Fresh authority rejects tokens from a prior authority (criterion 5)",
          "[unit][permission][approval][criterion-5]") {
  const auto first = make_authority();
  const auto rotated = make_authority();

  const auto now = fixed_base();
  const auto token =
      first.issue(ApprovalRequest{.tool_name = "ShellExec", .input = "ls /tmp", .identity = "operator"}, now);

  // The token is valid (round-trips on the issuing authority)…
  REQUIRE(first.verify(token, "ShellExec", "ls /tmp", "operator", now).has_value());
  // …but the rotated authority's MAC will not match.
  const auto r = rotated.verify(token, "ShellExec", "ls /tmp", "operator", now);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(reason_of(r.error()) == "mac_mismatch");
}

TEST_CASE("ApprovalAuthority::input_hash matches issue path", "[unit][permission][approval]") {
  const auto authority = make_authority();
  const auto now = fixed_base();
  const std::string_view input = "the quick brown fox jumps over the lazy dog";
  const auto token =
      authority.issue(ApprovalRequest{.tool_name = "FileRead", .input = input, .identity = "alice"}, now);

  REQUIRE(token.input_hash == ApprovalAuthority::input_hash(input));
  REQUIRE_FALSE(token.input_hash == ApprovalAuthority::input_hash("different input"));
}

TEST_CASE("ApprovalAuthority issues unique nonces", "[unit][permission][approval]") {
  const auto authority = make_authority();
  const auto now = fixed_base();
  const auto t1 = authority.issue(ApprovalRequest{.tool_name = "FileRead", .input = "x", .identity = "alice"}, now);
  const auto t2 = authority.issue(ApprovalRequest{.tool_name = "FileRead", .input = "x", .identity = "alice"}, now);

  // Same tool/input/identity, but the nonce is fresh per issue and the
  // MAC therefore differs.
  REQUIRE(t1.nonce != t2.nonce);
  REQUIRE(t1.mac != t2.mac);
  REQUIRE(authority.verify(t1, "FileRead", "x", "alice", now).has_value());
  REQUIRE(authority.verify(t2, "FileRead", "x", "alice", now).has_value());
}
