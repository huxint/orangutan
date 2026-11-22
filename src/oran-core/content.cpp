// src/oran-core/content.cpp — `Content` variant helpers.

#include <oran/core/content.hpp>

#include <optional>
#include <string_view>
#include <variant>

namespace orangutan::core {

bool holds_text(const Content& c) noexcept {
  return std::holds_alternative<TextContent>(c);
}

bool holds_thinking(const Content& c) noexcept {
  return std::holds_alternative<ThinkingContent>(c);
}

bool holds_tool_use(const Content& c) noexcept {
  return std::holds_alternative<ToolUseContent>(c);
}

bool holds_tool_result(const Content& c) noexcept {
  return std::holds_alternative<ToolResultContent>(c);
}

std::optional<std::string_view> text_view(const Content& c) noexcept {
  if (const auto* t = std::get_if<TextContent>(&c)) {
    return std::string_view{t->text};
  }
  return std::nullopt;
}

}  // namespace orangutan::core
