// include/oran/core/error.hpp — cross-boundary error type.
//
// Slice-0 public surface. See docs/rules/error-handling.md for the contract
// and docs/design-docs/core-beliefs.md for why `Result<T>` is the single
// boundary type.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/enum_names.hpp>

namespace orangutan::core {

enum class ErrorKind : std::uint8_t {
  ok,
  cancelled,
  invalid_argument,
  not_found,
  permission_denied,
  capability_not_granted,
  config,
  auth,
  io,
  network,
  rate_limit,
  upstream,
  parsing,
  timeout,
  conflict,
  not_modified,
  storage,
  hook_timeout,
  hook_failed,
  mailbox_overflowed,
  internal,
};

/// `Error` is the cross-library failure type — always paired with `Result<T>`
/// from `result.hpp`. Construct via the static builders to keep messages
/// shaped; attach structured context with `.with(key, value)`.
class Error {
public:
  using ContextEntry = std::pair<std::string, std::string>;

  Error(ErrorKind kind, std::string message) noexcept;

  [[nodiscard]] ErrorKind kind() const noexcept {
    return kind_;
  }
  [[nodiscard]] std::string_view message() const noexcept {
    return message_;
  }
  [[nodiscard]] std::span<const ContextEntry> context() const noexcept {
    return std::span<const ContextEntry>{context_};
  }
  [[nodiscard]] std::optional<std::chrono::milliseconds> retry_after() const noexcept {
    return retry_after_;
  }

  /// `true` if the kind is in the retry-friendly set (network, rate_limit,
  /// timeout, upstream). Callers must still respect `retry_after` if set.
  [[nodiscard]] bool retryable() const noexcept;

  /// Fluent context attachment. Returns *this so callsites read top-down.
  Error& with(std::string key, std::string value) &;
  Error&& with(std::string key, std::string value) &&;
  Error& with_retry_after(std::chrono::milliseconds) & noexcept;
  Error&& with_retry_after(std::chrono::milliseconds) && noexcept;

  /// Builders for the common categories. Add a builder when a callsite needs
  /// a category more than once — searching for the builder is faster than
  /// searching for stringly-typed `Error(kind::..., "...")` callsites.
  [[nodiscard]] static Error cancelled();
  [[nodiscard]] static Error invalid_argument(std::string message);
  [[nodiscard]] static Error not_found(std::string message);
  [[nodiscard]] static Error permission_denied(std::string message);
  [[nodiscard]] static Error config(std::string message);
  [[nodiscard]] static Error io(std::string message);
  [[nodiscard]] static Error network(std::string message);
  [[nodiscard]] static Error rate_limit(std::string message);
  [[nodiscard]] static Error upstream(std::string message);
  [[nodiscard]] static Error timeout(std::chrono::milliseconds elapsed);
  [[nodiscard]] static Error parsing(std::string message);
  [[nodiscard]] static Error not_modified(std::string message);
  [[nodiscard]] static Error storage(std::string message);
  [[nodiscard]] static Error internal(std::string message);

private:
  ErrorKind kind_{ErrorKind::internal};
  std::string message_;
  std::vector<ContextEntry> context_;
  std::optional<std::chrono::milliseconds> retry_after_;
};

}  // namespace orangutan::core

/// `std::format` support for `ErrorKind` and `Error`. Keeps the type usable
/// from `std::print(...)` without callers having to reach for `enum_name` or
/// hand-roll context formatting.
template <>
struct std::formatter<orangutan::core::ErrorKind> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::core::ErrorKind k, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(k), ctx);
  }
};

template <>
struct std::formatter<orangutan::core::Error> {
  constexpr auto parse(std::format_parse_context& ctx) {
    return ctx.begin();
  }

  template <class FormatContext>
  auto format(const orangutan::core::Error& e, FormatContext& ctx) const {
    auto out = std::format_to(ctx.out(), "{}: {}", e.kind(), e.message());
    for (const auto& [key, value] : e.context()) {
      out = std::format_to(out, " [{}={}]", key, value);
    }
    if (auto retry = e.retry_after()) {
      out = std::format_to(out, " (retry_after={}ms)", retry->count());
    }
    return out;
  }
};
