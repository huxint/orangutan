// bench/core/scenarios/error_construct.cpp
//
// A-vs-B comparison: building `Error` via the fluent builder + `.with` chain
// vs. building via the move-only constructor with a pre-filled context
// vector. Required by docs/rules/testing-and-bench.md — bench buckets must
// own at least one A/B comparison documenting the tradeoff.

#include <nanobench.h>

#include <string>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

using orangutan::core::Error;
using orangutan::core::ErrorKind;

namespace orangutan::bench {

namespace {

[[gnu::noinline]] Error make_via_builder(int attempt) {
  return Error::network("HTTP 503 from upstream")
      .with("agent", "primary")
      .with("model", "claude-opus-4-7")
      .with("attempt", std::to_string(attempt));
}

[[gnu::noinline]] Error make_via_prefilled(int attempt) {
  Error e{ErrorKind::network, "HTTP 503 from upstream"};
  e.with("agent", "primary");
  e.with("model", "claude-opus-4-7");
  e.with("attempt", std::to_string(attempt));
  return e;
}

}  // namespace

void register_error_construct(ankerl::nanobench::Bench& bench) {
  int counter = 0;
  bench.run("Error.builder_chain", [&] {
    auto e = make_via_builder(counter++);
    ankerl::nanobench::doNotOptimizeAway(e);
  });
  bench.run("Error.prefilled_ctor", [&] {
    auto e = make_via_prefilled(counter++);
    ankerl::nanobench::doNotOptimizeAway(e);
  });
}

}  // namespace orangutan::bench
