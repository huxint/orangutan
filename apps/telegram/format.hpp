#pragma once

#include <nlohmann/json.hpp>
#include <oran/core/result.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::telegram_host {
struct FormattedText {
  std::string text;
  nlohmann::json entities = nlohmann::json::array();
};

/// CommonMark becomes plain text plus Telegram UTF-16 entity ranges.
/// Raw HTML remains literal text. No HTML/Markdown parse mode is sent to Telegram.
[[nodiscard]] core::Result<FormattedText> format_markdown(std::string_view markdown);
/// Split text and clip/rebase entities without breaking UTF-8 or losing formatting.
[[nodiscard]] core::Result<std::vector<FormattedText>> split_formatted(const FormattedText& text,
                                                                       std::size_t max_bytes = 4000);
}  // namespace orangutan::telegram_host
