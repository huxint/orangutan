// bench/core/scenarios/time.cpp
//
// A-vs-B coverage for `core::time`:
//
//   1. `time_format_explicit`         : `core::time::format_iso8601_utc` using
//                                       the project's hand-shaped
//                                       `std::format("{:04}-...")` template.
//   2. `time_format_chrono_format`    : `std::format("{:%FT%T}Z", ...)`
//                                       against the same time point, using
//                                       chrono format specifiers.
//
// Both produce the same `YYYY-MM-DDTHH:MM:SS.fffZ` shape; the benchmark
// documents the cost difference so future memory/audit hot paths can pick the
// approach with eyes open.
//
//   3. `time_parse_iso8601_utc`       : the strict parser on a representative
//                                       millisecond-precision input.
//
// Pairs with `time_format_explicit` to document the parse-vs-format cost ratio
// for downstream callers that round-trip wire timestamps.

#include <nanobench.h>

#include <chrono>
#include <format>
#include <string>

#include <oran/core/time.hpp>

namespace orangutan::bench {

namespace {

using core::Time;
namespace ct = core::time;

[[gnu::noinline]] std::string format_explicit(Time t) {
  return ct::format_iso8601_utc(t);
}

[[gnu::noinline]] std::string format_chrono(Time t) {
  using namespace std::chrono;
  const auto tp_ms = floor<milliseconds>(t.to_system_time_point());
  return std::format("{:%FT%T}Z", tp_ms);
}

[[gnu::noinline]] Time parse_one(std::string_view s) {
  auto r = ct::parse_iso8601_utc(s);
  if (!r) {
    return Time::epoch();
  }
  return *r;
}

}  // namespace

void register_time_scenarios(ankerl::nanobench::Bench& bench) {
  using namespace std::chrono;
  const Time sample{sys_days{year{2026} / May / 16} + hours{11} + minutes{22} + seconds{33} + milliseconds{456}};

  bench.run("core.time_format_explicit", [&] {
    auto s = format_explicit(sample);
    ankerl::nanobench::doNotOptimizeAway(s);
  });
  bench.run("core.time_format_chrono_format", [&] {
    auto s = format_chrono(sample);
    ankerl::nanobench::doNotOptimizeAway(s);
  });

  const std::string canonical = "2026-05-16T11:22:33.456Z";
  bench.run("core.time_parse_iso8601_utc", [&] {
    auto t = parse_one(canonical);
    ankerl::nanobench::doNotOptimizeAway(t);
  });
}

}  // namespace orangutan::bench
