// src/oran-tool/memory_recall.cpp - `MemoryRecall` built-in.

#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
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

constexpr std::size_t kDefaultMemoryRecallLimit = 5;
constexpr std::size_t kMaxMemoryRecallLimit = 20;
constexpr auto kMemoryRecallFields = std::to_array<std::string_view>({"query", "limit", "kinds", "id", "offset"});

constexpr std::string_view kMemoryRecallSchema =
    R"({"type":"object","properties":{)"
    R"("query":{"type":"string","minLength":1,"description":"Literal topic words for lexical search, up to 4096 UTF-8 bytes. Omit id and offset. Omit both query and id to browse the index.","maxLength":4096},)"
    R"("id":{"type":"string","minLength":1,"description":"Exact note ID from this scope, typically discovered in the index. Omit query and offset."},)"
    R"("limit":{"type":"integer","minimum":1,"maximum":20,"description":"Maximum results. When omitted: 20 index entries, 5 search hits, or 1 exact-ID result."},)"
    R"("offset":{"type":"integer","minimum":0,"description":"Index browsing only: omission starts at 0. Continue with a returned next_offset and the same kind filters. Omit id and query."},)"
    R"("kinds":{"type":"array","items":{"type":"string","enum":["user","feedback","project","reference","team"]},"uniqueItems":true,"default":[],"description":"Filter by stored note kind; omitted or empty includes every kind."}},)"
    R"("required":[],"additionalProperties":false})";

[[nodiscard]] core::Result<std::size_t> parse_limit(const json& parsed, std::size_t default_limit) {
  const auto it = parsed.find("limit");
  if (it == parsed.end()) {
    return default_limit;
  }
  if (!it->is_number_unsigned()) {
    return std::unexpected(
        core::Error::invalid_argument("MemoryRecall: `limit` must be a positive integer").with("field", "limit"));
  }
  const auto value = it->get<std::uint64_t>();
  if (value == 0 || value > kMaxMemoryRecallLimit) {
    return std::unexpected(
        core::Error::invalid_argument("MemoryRecall: `limit` must be between 1 and 20").with("field", "limit"));
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] core::Result<std::vector<std::string>> parse_kinds(const json& parsed) {
  auto kinds = std::vector<std::string>{};
  const auto it = parsed.find("kinds");
  if (it == parsed.end()) {
    return kinds;
  }
  if (!it->is_array()) {
    return std::unexpected(
        core::Error::invalid_argument("MemoryRecall: `kinds` must be an array").with("field", "kinds"));
  }
  kinds.reserve(it->size());
  for (std::size_t i = 0; i < it->size(); ++i) {
    const auto& item = (*it)[i];
    if (!item.is_string()) {
      return std::unexpected(core::Error::invalid_argument("MemoryRecall: kind must be a string")
                                 .with("field", "kinds")
                                 .with("index", std::to_string(i)));
    }
    auto kind = item.get<std::string>();
    if (kind.empty()) {
      return std::unexpected(core::Error::invalid_argument("MemoryRecall: kind must be non-empty")
                                 .with("field", "kinds")
                                 .with("index", std::to_string(i)));
    }
    if (!std::ranges::contains(detail::kMemoryKinds, std::string_view{kind})) {
      return std::unexpected(
          core::Error::invalid_argument("MemoryRecall: unknown kind").with("field", "kinds").with("kind", kind));
    }
    if (std::ranges::contains(kinds, kind)) {
      return std::unexpected(core::Error::invalid_argument("MemoryRecall: kind filters must be unique")
                                 .with("field", "kinds")
                                 .with("kind", kind));
    }
    kinds.push_back(std::move(kind));
  }
  return kinds;
}

[[nodiscard]] core::Result<MemoryRecallRequest> parse_recall(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kMemoryRecallName, kMemoryRecallFields);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  if ((parsed->contains("query") && parsed->contains("id")) ||
      (parsed->contains("offset") && (parsed->contains("query") || parsed->contains("id")))) {
    return std::unexpected(core::Error::invalid_argument("MemoryRecall: choose index browsing, query, or id"));
  }

  std::string query;
  if (parsed->contains("query")) {
    auto value = detail::require_string_field(*parsed, kMemoryRecallName, "query");
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    if (auto valid = detail::validate_memory_text(*value, "query", true); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    if (value->size() > 4096) {
      return std::unexpected(core::Error::invalid_argument("MemoryRecall: query must not exceed 4096 bytes"));
    }
    query = std::move(*value);
  }

  std::string id;
  if (parsed->contains("id")) {
    auto value = detail::require_string_field(*parsed, kMemoryRecallName, "id");
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    if (auto valid = detail::validate_memory_text(*value, "id"); !valid) {
      return std::unexpected(std::move(valid).error());
    }
    id = std::move(*value);
  }
  std::size_t offset = 0;
  if (parsed->contains("offset")) {
    const auto& value = (*parsed)["offset"];
    if (!value.is_number_unsigned() ||
        value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 100) {
      return std::unexpected(
          core::Error::invalid_argument("MemoryRecall: offset must be a nonnegative bounded integer"));
    }
    offset = value.get<std::size_t>();
  }

  const auto default_limit = !id.empty() ? 1 : (query.empty() ? kMaxMemoryRecallLimit : kDefaultMemoryRecallLimit);
  auto limit = parse_limit(*parsed, default_limit);
  if (!limit) {
    return std::unexpected(std::move(limit).error());
  }
  auto kinds = parse_kinds(*parsed);
  if (!kinds) {
    return std::unexpected(std::move(kinds).error());
  }

  return MemoryRecallRequest{
      .query = std::move(query),
      .limit = *limit,
      .kinds = std::move(*kinds),
      .id = std::move(id),
      .offset = offset,
  };
}

[[nodiscard]] async::Awaitable<core::Result<Output>> memory_recall_handler(MemoryRecallRequest request,
                                                                           DispatchContext& ctx) {
  if (!ctx.memory_recall) {
    co_return std::unexpected(core::Error::invalid_argument("MemoryRecall: runtime service is not available")
                                  .with("reason", "memory_runtime_unavailable"));
  }

  co_return co_await ctx.memory_recall(std::move(request), ctx);
}

}  // namespace

core::Result<void> register_memory_recall(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kMemoryRecallName},
      .description = "Read saved knowledge when a past preference, correction, or decision is relevant to the current "
                     "request, or the user asks about memory. Use relevant facts to answer the request. "
                     "Do not announce routine lookups or volunteer note recaps. Ignore unrelated memories. "
                     "Call {} to browse a compact index, {\"id\":\"note-id\"} to read a relevant note in full, "
                     "or {\"query\":\"topic words\"} for full matching notes. Search is lexical: try different "
                     "topic words or browse if wording misses. Index cues are incomplete; read applicable notes "
                     "before relying on them. Follow next_offset with the same filters. The host supplies scope.",
      .input_schema_json = std::string{kMemoryRecallSchema},
      .required_capabilities = {},
  };

  return registry.add_prepared(
      std::move(def),
      [](std::string_view input_json) -> core::Result<PreparedCall> {
        auto parsed = parse_recall(input_json);
        if (!parsed) {
          return std::unexpected(std::move(parsed).error());
        }
        return PreparedCall{.path = std::nullopt,
                            .execute = [request = std::move(*parsed)](DispatchContext& ctx) mutable {
                              return memory_recall_handler(std::move(request), ctx);
                            }};
      },
      DispatchPolicy::runtime);
}

}  // namespace orangutan::tool
