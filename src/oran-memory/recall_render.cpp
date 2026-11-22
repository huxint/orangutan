#include <oran/memory/longterm.hpp>

#include <expected>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/time.hpp>

#include "_impl/text.hpp"

namespace orangutan::memory::longterm {
namespace {

void append_string_list(std::string& out, std::string_view label, std::span<const std::string> values) {
  if (values.empty()) {
    return;
  }
  std::format_to(std::back_inserter(out), "  {}: ", label);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      std::format_to(std::back_inserter(out), ", ");
    }
    std::format_to(std::back_inserter(out), "{}", values[i]);
  }
  std::format_to(std::back_inserter(out), "\n");
}

[[nodiscard]] nlohmann::json string_list_json(std::span<const std::string> values) {
  auto out = nlohmann::json::array();
  for (const auto& value : values) {
    out.push_back(value);
  }
  return out;
}

[[nodiscard]] nlohmann::json record_json(const Record& record) {
  return nlohmann::json{
      {"id", record.key.id},
      {"scope_key", record.key.scope_key},
      {"kind", std::string{core::enum_name(record.kind)}},
      {"title", record.title},
      {"body", record.body},
      {"created_at", core::time::format_iso8601_utc(record.created_at)},
      {"updated_at", core::time::format_iso8601_utc(record.updated_at)},
      {"last_read_at", core::time::format_iso8601_utc(record.last_read_at)},
      {"importance", record.importance},
      {"tags", string_list_json(record.tags)},
      {"linked_record_ids", string_list_json(record.linked_record_ids)},
      {"shadow", record.shadow},
  };
}

[[nodiscard]] nlohmann::json record_json(const SearchHit& hit) {
  auto out = record_json(hit.record);
  out["score"] = hit.score;
  return out;
}

}  // namespace

std::string render_recall_text(std::span<const SearchHit> hits) {
  if (hits.empty()) {
    return {};
  }

  std::string text;
  std::format_to(std::back_inserter(text), "Long-term memory:\n");
  for (const auto& hit : hits) {
    const auto& record = hit.record;
    std::format_to(std::back_inserter(text),
                   "- [{}] {} (id: {})\n",
                   core::enum_name(record.kind),
                   record.title,
                   record.key.id);
    std::format_to(std::back_inserter(text), "  {}\n", detail::flatten(record.body));
    append_string_list(text, "tags", record.tags);
    append_string_list(text, "linked", record.linked_record_ids);
  }
  return text;
}

std::string render_recall_data_json(std::span<const SearchHit> hits) {
  auto records = nlohmann::json::array();
  for (const auto& hit : hits) {
    records.push_back(record_json(hit));
  }
  return nlohmann::json{
      {"kind", "memory_recall"},
      {"match_count", hits.size()},
      {"records", std::move(records)},
  }
      .dump();
}

std::string render_remember_data_json(const Record& record) {
  return nlohmann::json{
      {"kind", "memory_remember"},
      {"record", record_json(record)},
  }
      .dump();
}

std::string render_forget_data_json(const RecordKey& key) {
  return nlohmann::json{
      {"kind", "memory_forget"},
      {"record",
       nlohmann::json{
           {"id", key.id},
           {"scope_key", key.scope_key},
       }},
  }
      .dump();
}

}  // namespace orangutan::memory::longterm
