// include/oran/core/message.hpp — author + ordered content blocks.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <oran/core/content.hpp>
#include <oran/core/role.hpp>
#include <oran/core/time.hpp>

namespace orangutan::core {

struct Message {
  Role role{Role::user};
  std::vector<Content> blocks;
  /// Optional ingest timestamp. Storage rows set this on `append`; in-memory
  /// fixtures may leave it unset.
  std::optional<Time> created_at;

  friend bool operator==(const Message&, const Message&) = default;

  /// Convenience factory: single `TextContent` block by `Role::user`.
  /// Intended for tests and bootstrap-side fixtures; production code should
  /// construct `Message{role, blocks}` directly so the multi-block shape is
  /// not lost.
  [[nodiscard]] static Message user_text(std::string text);

  /// Convenience factory: single `TextContent` block by `Role::assistant`.
  /// Same caveat as `user_text`.
  [[nodiscard]] static Message assistant_text(std::string text);
};

}  // namespace orangutan::core
