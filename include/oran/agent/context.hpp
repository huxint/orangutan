#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>

#include <oran/core/result.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::agent {

struct ContextOptions {
  /// Must fit every target in the configured route. Includes reserved output.
  std::size_t max_tokens{131072};
  std::size_t summary_max_bytes{8192};
  /// Optional model-aware input count. Default uses bytes plus framing overhead,
  /// conservatively treating each UTF-8 byte as one token.
  std::function<std::size_t(const provider::Request&)> count_tokens{};
};

[[nodiscard]] std::size_t estimate_input_tokens(const provider::Request& request);
[[nodiscard]] core::Result<void> validate_context_options(const ContextOptions& options);
[[nodiscard]] core::Result<void> validate_context_summary(std::string_view summary, std::size_t max_bytes);

}  // namespace orangutan::agent
