#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/permission.hpp>

namespace perm = orangutan::permission;
namespace core = orangutan::core;
using core::ErrorKind;
using perm::ApprovalBroker;
using perm::ApprovalGrant;

namespace {

[[nodiscard]] ApprovalBroker make_broker() {
  auto r = ApprovalBroker::with_random_secret();
  REQUIRE(r.has_value());
  return std::move(*r);
}

[[nodiscard]] core::Time fixed_base() noexcept {
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

TEST_CASE("ApprovalBroker round-trips approve + check", "[unit][permission][approval_broker]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  const auto token = broker.approve(ApprovalGrant{.tool_name = "ShellExec",
                                                  .input = "ls /tmp",
                                                  .identity = "operator",
                                                  .ttl = std::chrono::seconds{60},
                                                  .replay_max = 3},
                                    now);

  REQUIRE(broker.outstanding_grants() == 1);
  const auto r = broker.check(token, "ShellExec", "ls /tmp", "operator", now);
  REQUIRE(r.has_value());
}

TEST_CASE("ApprovalBroker honors replay_max then exhausts", "[unit][permission][approval_broker]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  const auto token =
      broker.approve(ApprovalGrant{.tool_name = "FileWrite", .input = "/tmp/a", .identity = "alice", .replay_max = 3},
                     now);

  for (int i = 0; i < 3; ++i) {
    const auto ok = broker.check(token, "FileWrite", "/tmp/a", "alice", now);
    INFO("iteration " << i);
    REQUIRE(ok.has_value());
  }

  const auto exhausted = broker.check(token, "FileWrite", "/tmp/a", "alice", now);
  REQUIRE_FALSE(exhausted.has_value());
  REQUIRE(exhausted.error().kind() == ErrorKind::permission_denied);
  REQUIRE(reason_of(exhausted.error()) == "replay_exhausted");
}

TEST_CASE("ApprovalBroker rejects valid tokens with no grant", "[unit][permission][approval_broker]") {
  // Two brokers share an authority via a hand-crafted setup: the first
  // approves, the second has the same authority but never saw the
  // approve call. The token verifies under both authorities (same
  // secret), so the second broker's rejection must come from the
  // missing entry, not the MAC.
  auto src = make_broker();
  const auto now = fixed_base();
  const auto token = src.approve(
      ApprovalGrant{.tool_name = "ShellExec", .input = "rm /tmp/x", .identity = "operator", .replay_max = 1},
      now);

  // Reap immediately — same effect as "broker has no grant".
  src.reap_expired(core::Time{now.to_system_time_point() + std::chrono::hours{2}});
  REQUIRE(src.outstanding_grants() == 0);

  const auto r = src.check(token, "ShellExec", "rm /tmp/x", "operator", now);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(reason_of(r.error()) == "no_grant");
}

TEST_CASE("ApprovalBroker re-approve resets the counter", "[unit][permission][approval_broker]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  const auto first = broker.approve(
      ApprovalGrant{.tool_name = "FileEdit", .input = "/etc/hosts", .identity = "alice", .replay_max = 1},
      now);

  REQUIRE(broker.check(first, "FileEdit", "/etc/hosts", "alice", now).has_value());
  REQUIRE(reason_of(broker.check(first, "FileEdit", "/etc/hosts", "alice", now).error()) == "replay_exhausted");

  // Re-approve the same triple — broker should overwrite the entry, and
  // the new token presents fresh remaining_uses.
  const auto second = broker.approve(
      ApprovalGrant{.tool_name = "FileEdit", .input = "/etc/hosts", .identity = "alice", .replay_max = 2},
      now);
  REQUIRE(broker.outstanding_grants() == 1);
  REQUIRE(broker.check(second, "FileEdit", "/etc/hosts", "alice", now).has_value());
  REQUIRE(broker.check(second, "FileEdit", "/etc/hosts", "alice", now).has_value());
  REQUIRE(reason_of(broker.check(second, "FileEdit", "/etc/hosts", "alice", now).error()) == "replay_exhausted");
}

TEST_CASE("ApprovalBroker::reap_expired drops past-TTL entries", "[unit][permission][approval_broker]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  static_cast<void>(broker.approve(
      ApprovalGrant{.tool_name = "FileRead", .input = "/tmp/a", .identity = "alice", .ttl = std::chrono::seconds{10}},
      now));
  static_cast<void>(broker.approve(
      ApprovalGrant{.tool_name = "FileRead", .input = "/tmp/b", .identity = "alice", .ttl = std::chrono::seconds{60}},
      now));
  REQUIRE(broker.outstanding_grants() == 2);

  // Step past the first TTL but inside the second.
  const auto t1 = core::Time{now.to_system_time_point() + std::chrono::seconds{30}};
  REQUIRE(broker.reap_expired(t1) == 1);
  REQUIRE(broker.outstanding_grants() == 1);

  // Step past the second TTL.
  const auto t2 = core::Time{now.to_system_time_point() + std::chrono::seconds{120}};
  REQUIRE(broker.reap_expired(t2) == 1);
  REQUIRE(broker.outstanding_grants() == 0);
}

TEST_CASE("ApprovalBroker honors replay_max=0 by rejecting immediately", "[unit][permission][approval_broker]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  const auto token =
      broker.approve(ApprovalGrant{.tool_name = "FileRead", .input = "/tmp/x", .identity = "alice", .replay_max = 0},
                     now);

  const auto r = broker.check(token, "FileRead", "/tmp/x", "alice", now);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(reason_of(r.error()) == "replay_exhausted");
}

TEST_CASE("ApprovalBroker keeps distinct triples independent", "[unit][permission][approval_broker]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  const auto a_token =
      broker.approve(ApprovalGrant{.tool_name = "FileWrite", .input = "/tmp/a", .identity = "alice", .replay_max = 1},
                     now);
  const auto b_token =
      broker.approve(ApprovalGrant{.tool_name = "FileWrite", .input = "/tmp/b", .identity = "alice", .replay_max = 1},
                     now);
  REQUIRE(broker.outstanding_grants() == 2);

  // Exhaust the first grant.
  REQUIRE(broker.check(a_token, "FileWrite", "/tmp/a", "alice", now).has_value());
  REQUIRE(reason_of(broker.check(a_token, "FileWrite", "/tmp/a", "alice", now).error()) == "replay_exhausted");

  // The second grant must still honor its counter.
  REQUIRE(broker.check(b_token, "FileWrite", "/tmp/b", "alice", now).has_value());
}

TEST_CASE("ApprovalBroker propagates authority-level errors without spending the honest counter",
          "[unit][permission][approval_broker]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  const auto token =
      broker.approve(ApprovalGrant{.tool_name = "FileWrite", .input = "/tmp/a", .identity = "alice", .replay_max = 2},
                     now);

  // Cross-tool attempt: the authority rejects before the broker's
  // counter is touched. We confirm both the reason forwarding and the
  // (honest-triple) counter integrity.
  const auto wrong_tool = broker.check(token, "ShellExec", "/tmp/a", "alice", now);
  REQUIRE_FALSE(wrong_tool.has_value());
  REQUIRE(reason_of(wrong_tool.error()) == "tool_mismatch");

  // Cross-input attempt — same property.
  const auto wrong_input = broker.check(token, "FileWrite", "/tmp/b", "alice", now);
  REQUIRE_FALSE(wrong_input.has_value());
  REQUIRE(reason_of(wrong_input.error()) == "input_mismatch");

  // The honest path should still have its full counter — 2 successful
  // checks in a row.
  REQUIRE(broker.check(token, "FileWrite", "/tmp/a", "alice", now).has_value());
  REQUIRE(broker.check(token, "FileWrite", "/tmp/a", "alice", now).has_value());
  REQUIRE(reason_of(broker.check(token, "FileWrite", "/tmp/a", "alice", now).error()) == "replay_exhausted");
}

TEST_CASE("ApprovalBroker rejects tokens after their TTL even if grant entry survives",
          "[unit][permission][approval_broker]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  const auto token = broker.approve(ApprovalGrant{.tool_name = "MemoryWrite",
                                                  .input = "fact-1",
                                                  .identity = "operator",
                                                  .ttl = std::chrono::seconds{10},
                                                  .replay_max = 5},
                                    now);

  // Inside TTL: passes.
  const auto inside = broker.check(token, "MemoryWrite", "fact-1", "operator", now);
  REQUIRE(inside.has_value());

  // After TTL: authority-level `expired` fires before the broker's
  // map lookup. The grant entry is still in the map (reap_expired is
  // explicit), but the verify path stops the use.
  const auto past = core::Time{now.to_system_time_point() + std::chrono::seconds{30}};
  const auto outside = broker.check(token, "MemoryWrite", "fact-1", "operator", past);
  REQUIRE_FALSE(outside.has_value());
  REQUIRE(reason_of(outside.error()) == "expired");
  REQUIRE(broker.outstanding_grants() == 1);
}

TEST_CASE("ApprovalBroker caps live grants per identity and evicts the oldest grant",
          "[unit][permission][approval_broker][bounded]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  std::vector<perm::ApprovalToken> tokens;
  tokens.reserve(ApprovalBroker::max_grants_per_identity + 1U);

  for (std::size_t i = 0; i <= ApprovalBroker::max_grants_per_identity; ++i) {
    tokens.push_back(broker.approve(
        ApprovalGrant{
            .tool_name = "FileWrite",
            .input = "path-" + std::to_string(i),
            .identity = "alice",
            .replay_max = 1,
        },
        now));
  }

  REQUIRE(broker.outstanding_grants() == ApprovalBroker::max_grants_per_identity);

  const auto evicted = broker.check(tokens.front(), "FileWrite", "path-0", "alice", now);
  REQUIRE_FALSE(evicted.has_value());
  REQUIRE(reason_of(evicted.error()) == "no_grant");

  const auto newest = broker.check(tokens.back(),
                                   "FileWrite",
                                   "path-" + std::to_string(ApprovalBroker::max_grants_per_identity),
                                   "alice",
                                   now);
  REQUIRE(newest.has_value());
}

TEST_CASE("ApprovalBroker grant ceiling is scoped per identity", "[unit][permission][approval_broker][bounded]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  const auto bob_token =
      broker.approve(ApprovalGrant{.tool_name = "FileWrite", .input = "bob-path", .identity = "bob", .replay_max = 1},
                     now);

  for (std::size_t i = 0; i <= ApprovalBroker::max_grants_per_identity; ++i) {
    static_cast<void>(broker.approve(
        ApprovalGrant{
            .tool_name = "FileWrite",
            .input = "alice-path-" + std::to_string(i),
            .identity = "alice",
            .replay_max = 1,
        },
        now));
  }

  REQUIRE(broker.outstanding_grants() == ApprovalBroker::max_grants_per_identity + 1U);
  REQUIRE(broker.check(bob_token, "FileWrite", "bob-path", "bob", now).has_value());
}

TEST_CASE("ApprovalBroker reaps expired grants before enforcing the identity ceiling",
          "[unit][permission][approval_broker][bounded]") {
  auto broker = make_broker();
  const auto now = fixed_base();
  static_cast<void>(broker.approve(
      ApprovalGrant{
          .tool_name = "FileWrite",
          .input = "expired",
          .identity = "alice",
          .ttl = std::chrono::seconds{1},
          .replay_max = 1,
      },
      now));

  const auto later = core::Time{now.to_system_time_point() + std::chrono::seconds{2}};
  std::vector<perm::ApprovalToken> tokens;
  tokens.reserve(ApprovalBroker::max_grants_per_identity);
  for (std::size_t i = 0; i < ApprovalBroker::max_grants_per_identity; ++i) {
    tokens.push_back(broker.approve(
        ApprovalGrant{
            .tool_name = "FileWrite",
            .input = "fresh-" + std::to_string(i),
            .identity = "alice",
            .replay_max = 1,
        },
        later));
  }

  REQUIRE(broker.outstanding_grants() == ApprovalBroker::max_grants_per_identity);
  const auto first_fresh = broker.check(tokens.front(), "FileWrite", "fresh-0", "alice", later);
  REQUIRE(first_fresh.has_value());
}
