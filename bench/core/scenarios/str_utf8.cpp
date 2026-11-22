// bench/core/scenarios/str_utf8.cpp
//
// A-vs-B coverage for `core::str::is_valid_utf8`:
//
//   1. `core.str_is_valid_utf8_mixed`        : the strict RFC-3629 walk on a
//                                              fixture containing ASCII +
//                                              2/3/4-byte sequences.
//   2. `core.str_ranges_all_of_ascii_only`   : a `std::ranges::all_of`
//                                              ASCII-only short-circuit on
//                                              the same fixture. Returns
//                                              early on the first non-ASCII
//                                              byte — the comparison
//                                              documents the cost of the
//                                              full validator vs. the
//                                              cheapest possible filter.
//
// Both scenarios receive a 1024-byte mixed fixture; the strict walk has to
// inspect every byte while the ASCII filter bails out almost immediately.
// The result describes the *upper bound* of validator cost over realistic
// content, so future call sites can pick when to validate vs. when to take
// an ASCII fast path first.

#include <nanobench.h>

#include <algorithm>
#include <string>
#include <string_view>

#include <oran/core/str.hpp>

namespace orangutan::bench {

namespace {

[[nodiscard]] std::string make_mixed_fixture() {
  // ~1024 bytes alternating ASCII / 2-byte / 3-byte / 4-byte code points.
  std::string out;
  out.reserve(1024);
  while (out.size() < 1024) {
    out.append("hello ");
    out.append("\xC2\xA9");                  // ©
    out.append("\xE4\xB8\xAD\xE6\x96\x87");  // 中文
    out.append("\xF0\x9F\x98\x80");          // 😀
    out.push_back(' ');
  }
  return out;
}

[[nodiscard]] bool ascii_only(std::string_view text) noexcept {
  return std::ranges::all_of(text, [](char c) noexcept { return static_cast<unsigned char>(c) < 0x80; });
}

}  // namespace

void register_str_utf8_scenarios(ankerl::nanobench::Bench& bench) {
  static const std::string fixture = make_mixed_fixture();

  bench.run("core.str_is_valid_utf8_mixed", [&] {
    auto ok = orangutan::core::str::is_valid_utf8(fixture);
    ankerl::nanobench::doNotOptimizeAway(ok);
  });
  bench.run("core.str_ranges_all_of_ascii_only", [&] {
    auto ok = ascii_only(fixture);
    ankerl::nanobench::doNotOptimizeAway(ok);
  });
}

}  // namespace orangutan::bench
