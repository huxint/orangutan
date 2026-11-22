// bench/permission/scenarios/approval_broker.cpp
//
// A-vs-B coverage for `ApprovalBroker`. The authority-level cost is already
// pinned by `bench/permission/scenarios/approval.cpp`; this bucket measures
// the *delta* the broker adds — the unordered-map lookup + counter
// decrement — and the cost of the two early-reject paths the authority
// can't see (`no_grant` and `replay_exhausted`).
//
//   1. `permission.broker_approve`            : `approve` path. Pays the
//                                                authority's full `issue`
//                                                cost plus one map insert
//                                                / overwrite (`operator[]`
//                                                + assignment, no hash
//                                                miss on the second
//                                                iteration onward — the
//                                                bench overwrites the
//                                                same triple). Documents
//                                                the steady-state grant
//                                                cost.
//   2. `permission.broker_check_ok`           : `check` path on a token
//                                                that round-trips. Pays
//                                                the authority's verify
//                                                cost plus one map find
//                                                + counter decrement.
//                                                The grant's
//                                                `replay_max` is set to
//                                                a deliberately large
//                                                value so the inner loop
//                                                never exhausts.
//   3. `permission.broker_check_no_grant`     : `check` after the entry
//                                                has been reaped. Verify
//                                                still pays full cost (the
//                                                authority can't tell the
//                                                grant is gone); the
//                                                broker pays one map
//                                                find-miss + a `reason=
//                                                no_grant` error build.
//                                                Documents the cost of
//                                                the broker's "lost the
//                                                grant" rejection.
//   4. `permission.broker_check_exhausted`    : `check` after the
//                                                counter has hit zero.
//                                                Same verify cost; the
//                                                broker pays one map
//                                                find-hit + a `reason=
//                                                replay_exhausted` error
//                                                build. Pair with #3 to
//                                                document why the broker
//                                                tracks remaining_uses
//                                                separately from
//                                                presence.

#include <chrono>
#include <string_view>

#include <nanobench.h>

#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using permission::ApprovalBroker;
using permission::ApprovalGrant;

[[nodiscard]] ApprovalBroker make_broker() {
  return *ApprovalBroker::with_random_secret();
}

[[nodiscard]] core::Time bench_now() noexcept {
  using namespace std::chrono;
  const auto ymd = year{2026} / January / day{1};
  return core::Time{sys_days{ymd}};
}

}  // namespace

void register_approval_broker_scenarios(ankerl::nanobench::Bench& b) {
  const auto now = bench_now();
  constexpr std::string_view tool = "ShellExec";
  constexpr std::string_view input = "ls -la /tmp/scratch";
  constexpr std::string_view identity = "operator";

  {
    auto broker = make_broker();
    b.run("permission.broker_approve", [&] {
      auto token = broker.approve(
          ApprovalGrant{.tool_name = tool, .input = input, .identity = identity, .replay_max = 1'000'000},
          now);
      ankerl::nanobench::doNotOptimizeAway(token);
    });
  }

  {
    auto broker = make_broker();
    // High `replay_max` keeps the inner loop honest — every iteration is
    // a successful check, so the bench measures the steady-state
    // verify + decrement cost rather than the transition to exhausted.
    const auto token = broker.approve(
        ApprovalGrant{.tool_name = tool, .input = input, .identity = identity, .replay_max = 1'000'000'000},
        now);
    b.run("permission.broker_check_ok", [&] {
      auto r = broker.check(token, tool, input, identity, now);
      ankerl::nanobench::doNotOptimizeAway(r.has_value());
    });
  }

  {
    auto broker = make_broker();
    const auto token =
        broker.approve(ApprovalGrant{.tool_name = tool, .input = input, .identity = identity, .replay_max = 1}, now);
    // Reap the entry: same token, no grant.
    broker.reap_expired(core::Time{now.to_system_time_point() + std::chrono::hours{2}});
    b.run("permission.broker_check_no_grant", [&] {
      auto r = broker.check(token, tool, input, identity, now);
      ankerl::nanobench::doNotOptimizeAway(r.has_value());
    });
  }

  {
    auto broker = make_broker();
    const auto token =
        broker.approve(ApprovalGrant{.tool_name = tool, .input = input, .identity = identity, .replay_max = 0}, now);
    // Grant exists but is already exhausted by construction.
    b.run("permission.broker_check_exhausted", [&] {
      auto r = broker.check(token, tool, input, identity, now);
      ankerl::nanobench::doNotOptimizeAway(r.has_value());
    });
  }
}

}  // namespace orangutan::bench
