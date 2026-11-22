// include/oran/core/content.hpp — typed message-block variant.
//
// `Content` is the per-block payload that flows through every higher layer.
// The variant alternatives are pure value types — strings stay opaque so
// protocol adapters (in `oran-provider`) can own JSON encoding/decoding
// without leaking nlohmann into the public core surface (rule C6).

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace orangutan::core {

struct TextContent {
  std::string text;

  friend bool operator==(const TextContent&, const TextContent&) = default;
};

/// Owned image input. Inline bytes keep credential-bearing download URLs out of
/// provider requests and make persisted conversations independent of remote files.
struct ImageContent {
  std::string media_type;
  std::string data_base64;
  friend bool operator==(const ImageContent&, const ImageContent&) = default;
};

struct ThinkingContent {
  std::string thinking;
  /// Optional vendor-issued signature (e.g., Anthropic extended thinking).
  std::optional<std::string> signature;

  friend bool operator==(const ThinkingContent&, const ThinkingContent&) = default;
};

struct ToolUseContent {
  /// Provider-issued unique id; round-trips back as `ToolResultContent::tool_use_id`.
  std::string id;
  std::string name;
  /// Opaque JSON document. `oran-provider` adapters own (de)serialization.
  std::string input_json;

  friend bool operator==(const ToolUseContent&, const ToolUseContent&) = default;
};

struct ToolResultContent {
  std::string tool_use_id;
  /// Tool output. Often plain text, occasionally JSON; opaque at this layer.
  std::string output;
  /// Optional structured tool output. When present, it is a serialized JSON
  /// document produced by the tool layer and preserved for provider adapters.
  std::optional<std::string> data_json;
  bool is_error{false};

  friend bool operator==(const ToolResultContent&, const ToolResultContent&) = default;
};

using Content = std::variant<TextContent, ThinkingContent, ToolUseContent, ToolResultContent, ImageContent>;

[[nodiscard]] bool holds_text(const Content&) noexcept;
[[nodiscard]] bool holds_thinking(const Content&) noexcept;
[[nodiscard]] bool holds_tool_use(const Content&) noexcept;
[[nodiscard]] bool holds_tool_result(const Content&) noexcept;

/// View of the inner `TextContent::text` when `c` holds a `TextContent`;
/// `std::nullopt` otherwise. The view points into `c`'s storage — callers must
/// keep `c` alive for the lifetime of the view.
[[nodiscard]] std::optional<std::string_view> text_view(const Content& c) noexcept;

}  // namespace orangutan::core
