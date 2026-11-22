#include <oran/memory/longterm.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/str.hpp>

#include "_impl/text.hpp"

namespace orangutan::memory::longterm {
namespace {

constexpr std::string_view kIndexHeader =
    "Memory index:\n"
    "Background context, not a user message. Use relevant notes quietly. "
    "Read with MemoryRecall {\"id\":\"...\"} before relying on a cue; search topic words if needed.\n";
constexpr std::size_t kFooterReserve = 192;

[[nodiscard]] std::string cue(std::string_view text, std::size_t max_bytes) {
  const auto prefix = core::str::truncate_to_code_point(text, max_bytes);
  auto out = detail::flatten(prefix);
  if (prefix.size() != text.size()) {
    out += "…";
  }
  return out;
}

}  // namespace

core::Result<IndexResult> make_index(std::span<const IndexEntry> candidates, const IndexRequest& request) {
  if (auto valid = validate_index_request(request); !valid) {
    return std::unexpected(std::move(valid).error());
  }
  auto result = IndexResult{.text = std::string{kIndexHeader}};
  auto& text = result.text;
  std::size_t consumed = 0;
  for (const auto& candidate : candidates.first(std::min(request.limit, candidates.size()))) {
    auto entry = IndexEntry{
        .key = candidate.key,
        .kind = candidate.kind,
        .title = cue(candidate.title, 120),
        .summary = cue(candidate.summary, 240),
    };
    const auto line = std::format("- [{}] {} (id: {})\n  {}\n",
                                  core::enum_name(entry.kind),
                                  entry.title,
                                  nlohmann::json(entry.key.id).dump(),
                                  entry.summary);
    if (line.size() > request.max_bytes - kIndexHeader.size() - kFooterReserve) {
      ++result.omitted_count;
      ++consumed;
      continue;
    }
    if (text.size() + line.size() + kFooterReserve > request.max_bytes) {
      break;
    }
    text += line;
    result.entries.push_back(std::move(entry));
    ++consumed;
  }
  if (consumed < candidates.size()) {
    result.next_offset = request.offset + consumed;
    std::format_to(std::back_inserter(text),
                   "More notes: call MemoryRecall with offset {} and the same kind filters.\n",
                   *result.next_offset);
  }
  if (result.omitted_count != 0) {
    std::format_to(std::back_inserter(text),
                   "{} oversized entries omitted; search their topics.\n",
                   result.omitted_count);
  }
  if (candidates.empty()) {
    text += request.offset == 0 ? "No saved notes match this index.\n" : "No more notes on this page.\n";
  }
  return result;
}

std::string render_index_data_json(const IndexResult& index) {
  auto entries = nlohmann::json::array();
  for (const auto& entry : index.entries) {
    entries.push_back(nlohmann::json{
        {"id", entry.key.id},
        {"scope_key", entry.key.scope_key},
        {"kind", core::enum_name(entry.kind)},
        {"title", entry.title},
        {"summary", entry.summary},
    });
  }
  return nlohmann::json{
      {"kind", "memory_index"},
      {"match_count", index.entries.size()},
      {"entries", std::move(entries)},
      {"next_offset", index.next_offset ? nlohmann::json(*index.next_offset) : nlohmann::json(nullptr)},
      {"omitted_count", index.omitted_count},
  }
      .dump();
}

}  // namespace orangutan::memory::longterm
