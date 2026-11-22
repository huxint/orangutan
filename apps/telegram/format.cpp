#include "format.hpp"

#include <algorithm>
#include <memory>
#include <unordered_map>

#include <cmark.h>
#include <oran/channel/adapter.hpp>

namespace orangutan::telegram_host {
namespace {
using Json = nlohmann::json;
std::size_t utf16_size(std::string_view text) {
  std::size_t length = 0;
  for (const unsigned char byte : text) {
    if (byte < 0x80 || byte >= 0xc0)
      length += byte >= 0xf0 ? 2 : 1;
  }
  return length;
}
bool link_url(std::string_view url) {
  return url.starts_with("https://") || url.starts_with("http://") || url.starts_with("mailto:");
}
void normalize_entities(FormattedText& formatted) {
  struct Range {
    std::size_t start, end;
  };
  const auto text_end = utf16_size(formatted.text);
  std::vector<Range> code;
  for (const auto& entity : formatted.entities) {
    if (entity.at("type") == "code" || entity.at("type") == "pre") {
      const auto start = entity.at("offset").get<std::size_t>();
      code.push_back({start, start + entity.at("length").get<std::size_t>()});
    }
  }
  std::ranges::sort(code, {}, &Range::start);
  Json entities = Json::array();
  for (const auto& entity : formatted.entities) {
    auto start = entity.at("offset").get<std::size_t>();
    const auto end = std::min(text_end, start + entity.at("length").get<std::size_t>());
    const auto emit = [&](std::size_t begin, std::size_t stop) {
      if (begin < stop) {
        auto part = entity;
        part["offset"] = begin;
        part["length"] = stop - begin;
        entities.push_back(std::move(part));
      }
    };
    if (entity.at("type") != "code" && entity.at("type") != "pre") {
      // Telegram forbids other entities from containing code/pre ranges.
      for (auto it = std::ranges::lower_bound(code, start, {}, &Range::end); it != code.end() && it->start < end;
           ++it) {
        emit(start, std::min(it->start, end));
        start = std::max(start, it->end);
      }
    }
    emit(start, end);
  }
  std::ranges::sort(entities.get_ref<Json::array_t&>(), [](const Json& left, const Json& right) {
    return left.at("offset") == right.at("offset") ? left.at("length") > right.at("length")
                                                   : left.at("offset") < right.at("offset");
  });
  formatted.entities = std::move(entities);
}
}  // namespace

core::Result<FormattedText> format_markdown(std::string_view markdown) {
  std::unique_ptr<cmark_node, decltype(&cmark_node_free)> root{
      cmark_parse_document(markdown.data(), markdown.size(), CMARK_OPT_VALIDATE_UTF8),
      cmark_node_free};
  if (!root)
    return std::unexpected(core::Error::internal("Markdown parser allocation failed"));
  std::unique_ptr<cmark_iter, decltype(&cmark_iter_free)> iterator{cmark_iter_new(root.get()), cmark_iter_free};
  if (!iterator)
    return std::unexpected(core::Error::internal("Markdown iterator allocation failed"));
  FormattedText result;
  std::size_t offset = 0;
  std::unordered_map<cmark_node*, Json> open;
  std::unordered_map<cmark_node*, int> lists;
  const auto append = [&](std::string_view text) {
    result.text += text;
    offset += utf16_size(text);
  };
  const auto start = [&](cmark_node* node, std::string_view type) -> Json& {
    return open[node] = Json{{"type", type}, {"offset", offset}};
  };
  const auto finish = [&](cmark_node* node) {
    auto found = open.find(node);
    if (found == open.end())
      return;
    auto entity = std::move(found->second);
    open.erase(found);
    const auto length = offset - entity.at("offset").get<std::size_t>();
    if (length != 0) {
      entity["length"] = length;
      result.entities.push_back(std::move(entity));
    }
  };
  for (auto event = cmark_iter_next(iterator.get()); event != CMARK_EVENT_DONE;
       event = cmark_iter_next(iterator.get())) {
    auto* node = cmark_iter_get_node(iterator.get());
    const auto type = cmark_node_get_type(node);
    if (event == CMARK_EVENT_EXIT) {
      finish(node);
      if (type == CMARK_NODE_PARAGRAPH || type == CMARK_NODE_HEADING || type == CMARK_NODE_BLOCK_QUOTE)
        append("\n\n");
      else if (type == CMARK_NODE_ITEM && !result.text.ends_with('\n'))
        append("\n");
      continue;
    }
    switch (type) {
      case CMARK_NODE_TEXT:
      case CMARK_NODE_HTML_INLINE:
      case CMARK_NODE_HTML_BLOCK:
        append(cmark_node_get_literal(node));
        break;
      case CMARK_NODE_SOFTBREAK:
      case CMARK_NODE_LINEBREAK:
        append("\n");
        break;
      case CMARK_NODE_CODE:
      case CMARK_NODE_CODE_BLOCK: {
        auto& entity = start(node, type == CMARK_NODE_CODE ? "code" : "pre");
        if (type == CMARK_NODE_CODE_BLOCK) {
          const std::string_view info{cmark_node_get_fence_info(node)};
          if (!info.empty())
            entity["language"] = info.substr(0, info.find_first_of(" \t\n"));
        }
        append(cmark_node_get_literal(node));
        finish(node);
        if (type == CMARK_NODE_CODE_BLOCK)
          append("\n");
        break;
      }
      case CMARK_NODE_EMPH:
        start(node, "italic");
        break;
      case CMARK_NODE_STRONG:
      case CMARK_NODE_HEADING:
        start(node, "bold");
        break;
      case CMARK_NODE_LINK:
      case CMARK_NODE_IMAGE: {
        const std::string_view url{cmark_node_get_url(node)};
        if (link_url(url))
          start(node, "text_link")["url"] = url;
        break;
      }
      case CMARK_NODE_BLOCK_QUOTE:
        // Telegram forbids nested blockquote entities; the outer quote owns it.
        if (!std::ranges::any_of(open, [](const auto& entry) { return entry.second.at("type") == "blockquote"; }))
          start(node, "blockquote");
        break;
      case CMARK_NODE_LIST:
        lists[node] = cmark_node_get_list_start(node);
        break;
      case CMARK_NODE_ITEM: {
        auto* list = cmark_node_parent(node);
        append(cmark_node_get_list_type(list) == CMARK_ORDERED_LIST ? std::to_string(lists[list]++) + ". " : "• ");
        break;
      }
      case CMARK_NODE_THEMATIC_BREAK:
        append("———\n");
        break;
      default:
        break;
    }
  }
  while (result.text.ends_with('\n'))
    result.text.pop_back();
  normalize_entities(result);
  return result;
}

core::Result<std::vector<FormattedText>> split_formatted(const FormattedText& text, std::size_t max_bytes) {
  auto parts = channel::split_text(text.text, max_bytes);
  if (!parts)
    return std::unexpected(parts.error());
  std::vector<FormattedText> result;
  std::size_t offset = 0;
  for (auto& part : *parts) {
    const auto end = offset + utf16_size(part);
    FormattedText formatted{.text = std::move(part)};
    for (auto entity : text.entities) {
      const auto start = entity.at("offset").get<std::size_t>();
      const auto stop = start + entity.at("length").get<std::size_t>();
      if (start < end && stop > offset) {
        entity["offset"] = std::max(start, offset) - offset;
        entity["length"] = std::min(stop, end) - std::max(start, offset);
        formatted.entities.push_back(std::move(entity));
      }
    }
    offset = end;
    result.push_back(std::move(formatted));
  }
  return result;
}
}  // namespace orangutan::telegram_host
