#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <oran/core/tool_def.hpp>

namespace orangutan::prompt {

struct RenderInputs {
  std::string_view system_preamble{};
  /// The same ordered definitions sent through the provider's native tools.
  /// Only name, description and schema contribute to the prefix fingerprint.
  std::span<const core::ToolDef> tools{};
  std::string_view skills_catalog{};
  std::string_view memory_framing{};
  std::string_view per_agent_overlay{};
};

struct RenderedPrompt {
  /// Nonempty stable text sections joined with one newline, ready for a request.
  std::string system_prompt{};
  std::uint64_t tool_catalog_hash{0};
  /// Fingerprints the joined system text and ordered native declarations.
  std::uint64_t prefix_hash{0};
  /// System text and native name/description/schema bytes, excluding protocol framing.
  std::size_t prefix_bytes{0};

  friend bool operator==(const RenderedPrompt&, const RenderedPrompt&) = default;
};

/// Render an owned stable prefix and cache identity from explicit values.
/// Native definitions affect the fingerprint; conversation stays in messages.
[[nodiscard]] RenderedPrompt render(RenderInputs inputs);

}  // namespace orangutan::prompt
