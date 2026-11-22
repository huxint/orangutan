#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <nanobench.h>

#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using permission::ApprovalSecret;

[[nodiscard]] ApprovalSecret fixed_secret() {
  std::array<std::byte, ApprovalSecret::key_bytes> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(i);
  }
  // Fixed-key factory only fails for size mismatch which `key` rules out.
  return *ApprovalSecret::from_bytes(key);
}

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view sv) noexcept {
  return {reinterpret_cast<const std::byte*>(sv.data()), sv.size()};
}

}  // namespace

void register_approval_secret_scenarios(ankerl::nanobench::Bench& b) {
  const auto secret = fixed_secret();

  // 32-byte payload, similar in size to a future canonical-token shape.
  const std::string short_msg(32, 'x');
  // 1 KiB payload — anchors per-byte cost growth.
  const std::string long_msg(1024, 'y');

  b.run("permission.hmac_short_message", [&] {
    auto m = secret.mac(as_bytes(short_msg));
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  b.run("permission.hmac_long_message", [&] {
    auto m = secret.mac(as_bytes(long_msg));
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  const auto ref = secret.mac(as_bytes(short_msg));
  auto mutated = ref;
  // Flip the last byte so `macs_equal` must walk to the end on the miss
  // path; constant-time compare costs the same as on the success path.
  mutated.back() = std::byte{static_cast<unsigned char>(std::to_integer<unsigned char>(mutated.back()) ^ 0xff)};

  b.run("permission.hmac_macs_equal_ok", [&] {
    bool eq = ApprovalSecret::macs_equal(ref, ref);
    ankerl::nanobench::doNotOptimizeAway(eq);
  });

  b.run("permission.hmac_macs_equal_no", [&] {
    bool eq = ApprovalSecret::macs_equal(ref, mutated);
    ankerl::nanobench::doNotOptimizeAway(eq);
  });
}

}  // namespace orangutan::bench
