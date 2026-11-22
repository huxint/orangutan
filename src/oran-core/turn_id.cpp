#include <oran/core/turn_id.hpp>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <random>
#include <string_view>

#include <oran/core/error.hpp>

namespace orangutan::core {

Result<TurnId> parse_turn_id_hex(std::string_view text) {
  TurnId id{};
  if (text.size() != id.size() * 2) {
    return std::unexpected(Error::invalid_argument("turn id must contain 32 lowercase hexadecimal characters"));
  }
  constexpr std::string_view DIGITS = "0123456789abcdef";
  for (std::size_t i = 0; i < id.size(); ++i) {
    const auto high = DIGITS.find(text[i * 2]);
    const auto low = DIGITS.find(text[i * 2 + 1]);
    if (high == std::string_view::npos || low == std::string_view::npos) {
      return std::unexpected(Error::invalid_argument("turn id must contain 32 lowercase hexadecimal characters"));
    }
    id[i] = static_cast<std::byte>((high << 4) | low);
  }
  return id;
}

namespace {

void write_u64_be(TurnId& id, std::size_t offset, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    const auto shift = static_cast<unsigned>((7 - index) * 8);
    id[offset + index] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

}  // namespace

std::string format_turn_id_hex(const TurnId& id) {
  constexpr std::string_view kHexDigits{"0123456789abcdef"};
  std::string out;
  out.reserve(id.size() * 2);
  for (auto byte : id) {
    const auto value = static_cast<unsigned char>(byte);
    out.push_back(kHexDigits[value >> 4]);
    out.push_back(kHexDigits[value & 0x0fu]);
  }
  return out;
}

Result<TurnId> generate_turn_id() {
  try {
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device random;
    const auto random_hi = static_cast<std::uint64_t>(random()) << 32U;
    const auto random_lo = static_cast<std::uint64_t>(random());
    const auto entropy = random_hi | random_lo;
    const auto counter = sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    const auto timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    TurnId id{};
    write_u64_be(id, 0, entropy ^ std::rotl(timestamp, 17));
    write_u64_be(id, 8, counter ^ std::rotl(entropy, 31) ^ timestamp);

    id[6] = (id[6] & std::byte{0x0f}) | std::byte{0x40};
    id[8] = (id[8] & std::byte{0x3f}) | std::byte{0x80};
    if (is_zero_turn_id(id)) {
      id[15] = std::byte{0x01};
    }
    return id;
  } catch (const std::exception& error) {
    return std::unexpected(Error::internal("failed to generate turn id").with("reason", error.what()));
  } catch (...) {
    return std::unexpected(Error::internal("failed to generate turn id").with("reason", "unknown"));
  }
}

}  // namespace orangutan::core
