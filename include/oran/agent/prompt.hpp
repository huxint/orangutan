#pragma once

#include <optional>
#include <string>
#include <vector>

#include <oran/core/content.hpp>
#include <oran/core/turn_id.hpp>

namespace orangutan::agent {

struct PromptRequest {
  std::string prompt;
  std::optional<core::TurnId> turn_id{};
  std::vector<core::ImageContent> images{};
  /// Initiating parent turn for reused child sessions; absent uses the session default.
  std::optional<core::TurnId> parent_turn_id{};
};

struct PromptResult {
  std::string text;
};

}  // namespace orangutan::agent
