#pragma once

#include <string>
#include <string_view>

namespace orangutan::memory::longterm::detail {

[[nodiscard]] constexpr bool is_space(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

/// Collapse whitespace runs to one space and trim both ends for one-line prompt text.
[[nodiscard]] inline std::string flatten(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pending_space = false;
  for (const auto ch : text) {
    if (is_space(ch)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(ch);
  }
  return out;
}

}  // namespace orangutan::memory::longterm::detail
