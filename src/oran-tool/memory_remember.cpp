// src/oran-tool/memory_remember.cpp - `MemoryRemember` built-in.

#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/tool/output.hpp>
#include <oran/tool/registry.hpp>

#include "_impl/memory_input.hpp"
#include "_impl/parse_input.hpp"

namespace orangutan::tool {
namespace {

using json = nlohmann::json;

constexpr double kDefaultMemoryImportance = 0.5;
constexpr auto kMemoryRememberFields =
    std::to_array<std::string_view>({"id", "kind", "title", "body", "importance", "tags", "linked_record_ids"});

constexpr std::string_view kMemoryRememberSchema =
    R"({"type":"object","properties":{)"
    R"("id":{"type":"string","description":"Stable, nonblank ID for one lesson. Reuse the existing ID to replace that note; a new ID creates a separate note.","minLength":1},)"
    R"("kind":{"type":"string","enum":["user","feedback","project","reference","team"],"description":"user: lasting user facts or preferences; feedback: correction or lesson; project: durable project decision; reference: useful pointer; team: shared working convention within this scope."},)"
    R"("title":{"type":"string","description":"Short, nonblank topic title used to decide relevance to future requests.","minLength":1},)"
    R"("body":{"type":"string","description":"Complete replacement note: one durable fact, why it matters, and when to apply it. The opening sentence becomes an index cue. Background knowledge, not a reply to the user.","minLength":1},)"
    R"("importance":{"type":"number","minimum":0,"maximum":1,"default":0.5,"description":"Relative priority for discovery, from 0 to 1; not confidence or proof of truth."},)"
    R"("tags":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true,"default":[],"description":"Unique nonblank topic labels for lexical discovery. Replaces existing tags; omission clears them."},)"
    R"("linked_record_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true,"default":[],"description":"Unique nonblank related note IDs in this scope. Replaces existing links; omission clears them."}},)"
    R"("required":["id","kind","title","body"],"additionalProperties":false})";

[[nodiscard]] core::Result<std::string> require_non_empty_string(const json& parsed, std::string_view field) {
  auto value = detail::require_string_field(parsed, kMemoryRememberName, field);
  if (!value) {
    return std::unexpected(std::move(value).error());
  }
  if (auto valid = detail::validate_memory_text(*value, field, field == "body"); !valid) {
    return std::unexpected(std::move(valid).error());
  }
  if (field == "kind" && !std::ranges::contains(detail::kMemoryKinds, std::string_view{*value})) {
    return std::unexpected(
        core::Error::invalid_argument("MemoryRemember: unknown kind").with("field", "kind").with("kind", *value));
  }
  return std::move(*value);
}

[[nodiscard]] core::Result<double> parse_importance(const json& parsed) {
  if (!parsed.contains("importance")) {
    return kDefaultMemoryImportance;
  }
  const auto& raw = parsed["importance"];
  if (!raw.is_number()) {
    return std::unexpected(
        core::Error::invalid_argument("MemoryRemember: `importance` must be a number").with("field", "importance"));
  }
  const auto value = raw.get<double>();
  if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
    return std::unexpected(core::Error::invalid_argument("MemoryRemember: `importance` must be in the range [0, 1]")
                               .with("field", "importance"));
  }
  return value;
}

[[nodiscard]] core::Result<std::vector<std::string>> parse_string_array(const json& parsed, std::string_view field) {
  auto values = std::vector<std::string>{};
  const auto key = std::string{field};
  if (!parsed.contains(key)) {
    return values;
  }
  const auto& raw = parsed[key];
  if (!raw.is_array()) {
    return std::unexpected(core::Error::invalid_argument("MemoryRemember: field must be an array").with("field", key));
  }
  values.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const auto& item = raw[i];
    if (!item.is_string()) {
      return std::unexpected(core::Error::invalid_argument("MemoryRemember: array item must be a string")
                                 .with("field", key)
                                 .with("index", std::to_string(i)));
    }
    auto value = item.get<std::string>();
    if (auto valid = detail::validate_memory_text(value, field); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    if (std::ranges::contains(values, value)) {
      return std::unexpected(core::Error::invalid_argument("MemoryRemember: array values must be unique")
                                 .with("field", key)
                                 .with("value", value));
    }
    values.push_back(std::move(value));
  }
  return values;
}

[[nodiscard]] core::Result<MemoryRememberRequest> parse_remember(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kMemoryRememberName, kMemoryRememberFields);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto id = require_non_empty_string(*parsed, "id");
  if (!id) {
    return std::unexpected(std::move(id).error());
  }
  auto kind = require_non_empty_string(*parsed, "kind");
  if (!kind) {
    return std::unexpected(std::move(kind).error());
  }
  auto title = require_non_empty_string(*parsed, "title");
  if (!title) {
    return std::unexpected(std::move(title).error());
  }
  auto body = require_non_empty_string(*parsed, "body");
  if (!body) {
    return std::unexpected(std::move(body).error());
  }
  auto importance = parse_importance(*parsed);
  if (!importance) {
    return std::unexpected(std::move(importance).error());
  }
  auto tags = parse_string_array(*parsed, "tags");
  if (!tags) {
    return std::unexpected(std::move(tags).error());
  }
  auto linked_record_ids = parse_string_array(*parsed, "linked_record_ids");
  if (!linked_record_ids) {
    return std::unexpected(std::move(linked_record_ids).error());
  }

  return MemoryRememberRequest{
      .id = std::move(*id),
      .kind = std::move(*kind),
      .title = std::move(*title),
      .body = std::move(*body),
      .importance = *importance,
      .tags = std::move(*tags),
      .linked_record_ids = std::move(*linked_record_ids),
  };
}

[[nodiscard]] async::Awaitable<core::Result<Output>> memory_remember_handler(MemoryRememberRequest request,
                                                                             DispatchContext& ctx) {
  if (!ctx.memory_remember) {
    co_return std::unexpected(core::Error::invalid_argument("MemoryRemember: runtime service is not available")
                                  .with("reason", "memory_runtime_unavailable")
                                  .with("id", request.id));
  }

  co_return co_await ctx.memory_remember(std::move(request), ctx);
}

}  // namespace

core::Result<void> register_memory_remember(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kMemoryRememberName},
      .description = "Persist one durable preference, correction, decision, or reference for later sessions. Save "
                     "it in the turn you learn it. Read an existing related note and reuse its id to correct it. "
                     "An update replaces the note's content and supplied metadata, not just changed fields; "
                     "preserve tags and links that still apply. Do not save guesses, secrets, one-off instructions, "
                     "task progress or facts easily re-read from code. The host supplies scope and timestamps. "
                     "Success confirms persistence. Routine saves need no announcement; briefly confirm an "
                     "explicit save request only after success.",
      .input_schema_json = std::string{kMemoryRememberSchema},
      .required_capabilities = {},
  };

  return registry.add_prepared(
      std::move(def),
      [](std::string_view input_json) -> core::Result<PreparedCall> {
        auto parsed = parse_remember(input_json);
        if (!parsed) {
          return std::unexpected(std::move(parsed).error());
        }
        return PreparedCall{.path = std::nullopt,
                            .execute = [request = std::move(*parsed)](DispatchContext& ctx) mutable {
                              return memory_remember_handler(std::move(request), ctx);
                            }};
      },
      DispatchPolicy::runtime);
}

}  // namespace orangutan::tool
