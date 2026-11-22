#include <chrono>
#include <string_view>

#include <nanobench.h>

#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using permission::ApprovalAuthority;
using permission::ApprovalRequest;

[[nodiscard]] ApprovalAuthority make_authority() {
  // `with_random_secret` only fails on a broken libsodium init, which
  // would have caught us long before this bench compiles.
  return *ApprovalAuthority::with_random_secret();
}

[[nodiscard]] core::Time bench_now() noexcept {
  using namespace std::chrono;
  const auto ymd = year{2026} / January / day{1};
  return core::Time{sys_days{ymd}};
}

}  // namespace

void register_approval_scenarios(ankerl::nanobench::Bench& b) {
  const auto authority = make_authority();
  const auto now = bench_now();
  const std::string_view input = "ls -la /tmp/scratch";

  b.run("permission.approval_issue", [&] {
    auto token =
        authority.issue(ApprovalRequest{.tool_name = "ShellExec", .input = input, .identity = "operator"}, now);
    ankerl::nanobench::doNotOptimizeAway(token);
  });

  const auto valid = authority.issue(ApprovalRequest{.tool_name = "ShellExec",
                                                     .input = input,
                                                     .identity = "operator",
                                                     .ttl = std::chrono::seconds{60}},
                                     now);

  b.run("permission.approval_verify_ok", [&] {
    auto r = authority.verify(valid, "ShellExec", input, "operator", now);
    ankerl::nanobench::doNotOptimizeAway(r.has_value());
  });

  const auto past_expiry = core::Time{valid.expires_at.to_system_time_point() + std::chrono::seconds{1}};
  b.run("permission.approval_verify_expired", [&] {
    auto r = authority.verify(valid, "ShellExec", input, "operator", past_expiry);
    ankerl::nanobench::doNotOptimizeAway(r.has_value());
  });
}

}  // namespace orangutan::bench
