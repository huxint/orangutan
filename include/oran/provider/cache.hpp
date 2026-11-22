#pragma once

#include <cstddef>
#include <cstdint>

namespace orangutan::provider {

/// Caller-owned identity for stable system text and native tool declarations.
/// Conversation is excluded; the selected protocol target decides eligibility.
struct PromptCacheHints {
  std::uint64_t prefix_hash{0};
  std::size_t prefix_bytes{0};

  friend bool operator==(const PromptCacheHints&, const PromptCacheHints&) = default;
};

struct PromptCacheOptions {
  bool enabled{true};
  std::size_t min_prefix_bytes{0};

  friend bool operator==(const PromptCacheOptions&, const PromptCacheOptions&) = default;
};

}  // namespace orangutan::provider
