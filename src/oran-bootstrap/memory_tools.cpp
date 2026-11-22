#include "memory_tools.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/hook.hpp>
#include <oran/memory/longterm.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::bootstrap {
namespace {

using core::Error;
using core::Result;

[[nodiscard]] Result<std::vector<memory::longterm::RecordKind>>
parse_memory_tool_recall_kinds(std::span<const std::string> names) {
  auto kinds = std::vector<memory::longterm::RecordKind>{};
  kinds.reserve(names.size());
  for (const auto& name : names) {
    auto parsed = core::parse_enum<memory::longterm::RecordKind>(name);
    if (!parsed) {
      return std::unexpected(Error::invalid_argument("MemoryRecall: unknown kind").with("kind", name));
    }
    if (std::ranges::contains(kinds, *parsed)) {
      return std::unexpected(Error::invalid_argument("MemoryRecall: kind filters must be unique").with("kind", name));
    }
    kinds.push_back(*parsed);
  }
  return kinds;
}

[[nodiscard]] Result<memory::longterm::RecordKind> parse_memory_tool_remember_kind(std::string_view name) {
  auto parsed = core::parse_enum<memory::longterm::RecordKind>(name);
  if (!parsed) {
    return std::unexpected(Error::invalid_argument("MemoryRemember: unknown kind").with("kind", std::string{name}));
  }
  return *parsed;
}

[[nodiscard]] std::string render_memory_recall_tool_text(const memory::longterm::RecallResult& recalled) {
  if (recalled.hits.empty()) {
    return "MemoryRecall: no matches";
  }

  std::string text;
  std::format_to(std::back_inserter(text),
                 "MemoryRecall: {} match{}\n",
                 recalled.hits.size(),
                 recalled.hits.size() == 1 ? "" : "es");
  text.append(recalled.text);
  return text;
}

[[nodiscard]] std::uintmax_t memory_record_payload_bytes(const memory::longterm::Record& record) noexcept {
  auto bytes = static_cast<std::uintmax_t>(record.key.id.size() + record.title.size() + record.body.size());
  for (const auto& tag : record.tags) {
    bytes += static_cast<std::uintmax_t>(tag.size());
  }
  for (const auto& id : record.linked_record_ids) {
    bytes += static_cast<std::uintmax_t>(id.size());
  }
  return bytes;
}

[[nodiscard]] std::chrono::nanoseconds duration_between(core::Time started_at, core::Time finished_at) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(finished_at.to_system_time_point() -
                                                              started_at.to_system_time_point());
}

[[nodiscard]] hook::Identity hook_identity(const tool::DispatchContext& ctx) {
  return hook::Identity{
      .scope_key = ctx.scope_key,
      .agent_key = ctx.agent_key,
      .identity = ctx.identity,
  };
}

[[nodiscard]] hook::MemoryRecordPayload hook_memory_record(const memory::longterm::Record& record) {
  return hook::MemoryRecordPayload{
      .id = record.key.id,
      .scope_key = record.key.scope_key,
      .kind = std::string{core::enum_name(record.kind)},
      .title = record.title,
      .body = record.body,
      .created_at = record.created_at,
      .updated_at = record.updated_at,
      .last_read_at = record.last_read_at,
      .importance = record.importance,
      .tags = record.tags,
      .linked_record_ids = record.linked_record_ids,
      .shadow = record.shadow,
  };
}

[[nodiscard]] hook::RedactedMemoryRecordPayload redacted_hook_memory_record(const memory::longterm::Record& record) {
  return hook::RedactedMemoryRecordPayload{
      .id = record.key.id,
      .scope_key = record.key.scope_key,
      .kind = std::string{core::enum_name(record.kind)},
      .title_bytes = record.title.size(),
      .body_bytes = record.body.size(),
      .tag_count = record.tags.size(),
      .linked_record_count = record.linked_record_ids.size(),
      .shadow = record.shadow,
  };
}

[[nodiscard]] hook::MemoryReadHitPayload hook_memory_read_hit(const memory::longterm::SearchHit& hit) {
  return hook::MemoryReadHitPayload{
      .record = hook_memory_record(hit.record),
      .score = hit.score,
      .redacted_record = redacted_hook_memory_record(hit.record),
  };
}

// Index observations carry only the displayed cue; undisplayed metadata stays neutral.
[[nodiscard]] hook::MemoryReadHitPayload hook_memory_index_hit(const memory::longterm::IndexEntry& entry) {
  const auto kind = std::string{core::enum_name(entry.kind)};
  return hook::MemoryReadHitPayload{
      .record =
          hook::MemoryRecordPayload{
              .id = entry.key.id,
              .scope_key = entry.key.scope_key,
              .kind = kind,
              .title = entry.title,
              .body = entry.summary,
          },
      .redacted_record =
          hook::RedactedMemoryRecordPayload{
              .id = entry.key.id,
              .scope_key = entry.key.scope_key,
              .kind = kind,
              .title_bytes = entry.title.size(),
              .body_bytes = entry.summary.size(),
          },
  };
}

[[nodiscard]] hook::MemoryWritePayload make_memory_write_payload(const memory::longterm::Record& record,
                                                                 const tool::DispatchContext& ctx,
                                                                 core::Time started_at,
                                                                 core::Time finished_at) {
  return hook::MemoryWritePayload{
      .who = hook_identity(ctx),
      .record = hook_memory_record(record),
      .redacted_record = redacted_hook_memory_record(record),
      .started_at = started_at,
      .finished_at = finished_at,
      .duration = duration_between(started_at, finished_at),
  };
}

[[nodiscard]] hook::MemoryReadPayload make_memory_read_payload(const tool::DispatchContext& context,
                                                               const tool::MemoryRecallRequest& request,
                                                               std::string source,
                                                               std::vector<hook::MemoryReadHitPayload> payload_hits,
                                                               core::Time started_at,
                                                               core::Time finished_at) {
  return hook::MemoryReadPayload{
      .who = hook_identity(context),
      .source = std::move(source),
      .query = request.query,
      .redacted_query_bytes = request.query.size(),
      .limit = request.limit,
      .kinds = request.kinds,
      .match_count = payload_hits.size(),
      .hits = std::move(payload_hits),
      .started_at = started_at,
      .finished_at = finished_at,
      .duration = duration_between(started_at, finished_at),
  };
}

[[nodiscard]] hook::MemoryForgetPayload make_memory_forget_payload(const memory::longterm::RecordKey& key,
                                                                   const tool::DispatchContext& ctx,
                                                                   core::Time started_at,
                                                                   core::Time finished_at) {
  return hook::MemoryForgetPayload{
      .who = hook_identity(ctx),
      .id = key.id,
      .scope_key = key.scope_key,
      .started_at = started_at,
      .finished_at = finished_at,
      .duration = duration_between(started_at, finished_at),
  };
}

[[nodiscard]] std::string hook_decision_reason(const hook::HookDecision& decision, std::string_view fallback) {
  return decision.reason.empty() ? std::string{fallback} : decision.reason;
}

[[nodiscard]] Error memory_write_hook_blocked_error(const hook::HookDecision& decision, std::string_view fallback) {
  return Error::permission_denied("memory write blocked by hook")
      .with("event", "memory_write_before")
      .with("reason", "blocked_by_hook")
      .with("decision_kind", std::string{core::enum_name(decision.kind)})
      .with("hook_reason", hook_decision_reason(decision, fallback));
}

[[nodiscard]] std::string render_memory_remember_tool_text(const memory::longterm::Record& record) {
  std::string text;
  std::format_to(std::back_inserter(text),
                 "MemoryRemember: saved {} record {}\n",
                 core::enum_name(record.kind),
                 record.key.id);
  std::format_to(std::back_inserter(text), "title: {}", record.title);
  return text;
}

[[nodiscard]] std::string render_memory_forget_tool_text(const memory::longterm::RecordKey& key) {
  return std::format("MemoryForget: removed record {}", key.id);
}

[[nodiscard]] async::Awaitable<Result<tool::Output>> recall_memory(memory::longterm::Backend& backend,
                                                                   std::string scope_key,
                                                                   tool::MemoryRecallRequest request,
                                                                   tool::DispatchContext& ctx) {
  auto kinds = parse_memory_tool_recall_kinds(std::span<const std::string>{request.kinds});
  if (!kinds) {
    co_return std::unexpected(std::move(kinds).error());
  }
  const auto started_at = core::time::now_utc();
  if (request.query.empty() && request.id.empty()) {
    const auto text_cap = ctx.output_caps.max_text_bytes;
    auto indexed = co_await asio::co_spawn(
        ctx.executor,
        memory::longterm::index(backend,
                                memory::longterm::IndexRequest{
                                    .scope_key = scope_key,
                                    .kinds = *kinds,
                                    .limit = request.limit,
                                    .offset = request.offset,
                                    .max_bytes = text_cap == 0 ? memory::longterm::kMaxIndexBytes
                                                               : std::min(text_cap, memory::longterm::kMaxIndexBytes),
                                }),
        asio::use_awaitable);
    if (!indexed) {
      co_return std::unexpected(std::move(indexed).error());
    }
    if (ctx.bus != nullptr) {
      auto cues = std::vector<hook::MemoryReadHitPayload>{};
      cues.reserve(indexed->entries.size());
      for (const auto& entry : indexed->entries) {
        cues.push_back(hook_memory_index_hit(entry));
      }
      co_await ctx.bus->publish_advisory(hook::Event::memory_read_after,
                                         make_memory_read_payload(ctx,
                                                                  request,
                                                                  "MemoryRecall:index",
                                                                  std::move(cues),
                                                                  started_at,
                                                                  core::time::now_utc()));
    }
    auto data = memory::longterm::render_index_data_json(*indexed);
    co_return tool::Output{
        .text = std::move(indexed->text),
        .data_json = std::move(data),
        .usage = tool::ToolUsage{.match_count = static_cast<std::uint64_t>(indexed->entries.size())},
    };
  }
  auto recalled = co_await asio::co_spawn(ctx.executor,
                                          memory::longterm::recall(backend,
                                                                   memory::longterm::RecallRequest{
                                                                       .query =
                                                                           memory::longterm::Query{
                                                                               .scope_key = scope_key,
                                                                               .text = request.query,
                                                                               .kinds = *kinds,
                                                                           },
                                                                       .limit = request.limit,
                                                                       .record_id = request.id,
                                                                   }),
                                          asio::use_awaitable);
  if (!recalled) {
    co_return std::unexpected(std::move(recalled).error());
  }
  if (ctx.bus != nullptr) {
    auto hits = std::vector<hook::MemoryReadHitPayload>{};
    hits.reserve(recalled->hits.size());
    for (const auto& hit : recalled->hits) {
      hits.push_back(hook_memory_read_hit(hit));
    }
    co_await ctx.bus->publish_advisory(
        hook::Event::memory_read_after,
        make_memory_read_payload(ctx, request, "MemoryRecall", std::move(hits), started_at, core::time::now_utc()));
  }
  auto data_json =
      memory::longterm::render_recall_data_json(std::span<const memory::longterm::SearchHit>{recalled->hits});
  co_return tool::Output{
      .text = render_memory_recall_tool_text(*recalled),
      .data_json = std::move(data_json),
      .attachments = {},
      .usage =
          tool::ToolUsage{
              .match_count = static_cast<std::uint64_t>(recalled->hits.size()),
          },
      .is_error = false,
  };
}

[[nodiscard]] async::Awaitable<Result<tool::Output>> remember_memory(memory::longterm::Backend& backend,
                                                                     std::string scope_key,
                                                                     tool::MemoryRememberRequest request,
                                                                     tool::DispatchContext& ctx) {
  auto kind = parse_memory_tool_remember_kind(request.kind);
  if (!kind) {
    co_return std::unexpected(std::move(kind).error());
  }

  auto record = memory::longterm::Record{
      .key = memory::longterm::RecordKey{.id = std::move(request.id), .scope_key = scope_key},
      .kind = *kind,
      .title = std::move(request.title),
      .body = std::move(request.body),
      .created_at = ctx.now,
      .updated_at = ctx.now,
      .last_read_at = ctx.now,
      .importance = request.importance,
      .tags = std::move(request.tags),
      .linked_record_ids = std::move(request.linked_record_ids),
  };

  const auto started_at = core::time::now_utc();
  if (ctx.bus != nullptr) {
    auto before = co_await ctx.bus->publish_blocking<hook::Event::memory_write_before>(
        make_memory_write_payload(record, ctx, started_at, started_at));
    if (!before) {
      auto error = std::move(before).error();
      error.with("event", "memory_write_before");
      co_return std::unexpected(std::move(error));
    }
    switch (before->kind) {
      case hook::HookDecisionKind::proceed:
        break;
      case hook::HookDecisionKind::veto:
        co_return std::unexpected(memory_write_hook_blocked_error(*before, "hook veto"));
      case hook::HookDecisionKind::rewrite:
        co_return std::unexpected(memory_write_hook_blocked_error(*before, "memory write rewrite unsupported"));
      case hook::HookDecisionKind::require_approval:
        co_return std::unexpected(
            memory_write_hook_blocked_error(*before, "memory write require_approval unsupported"));
    }
  }

  auto stored = co_await asio::co_spawn(ctx.executor, backend.upsert(std::move(record)), asio::use_awaitable);
  if (!stored) {
    co_return std::unexpected(std::move(stored).error());
  }
  if (ctx.bus != nullptr) {
    const auto finished_at = core::time::now_utc();
    co_await ctx.bus->publish_advisory(hook::Event::memory_write_after,
                                       make_memory_write_payload(*stored, ctx, started_at, finished_at));
  }
  auto data_json = memory::longterm::render_remember_data_json(*stored);
  co_return tool::Output{
      .text = render_memory_remember_tool_text(*stored),
      .data_json = std::move(data_json),
      .attachments = {},
      .usage =
          tool::ToolUsage{
              .bytes_written = memory_record_payload_bytes(*stored),
          },
      .is_error = false,
  };
}

[[nodiscard]] async::Awaitable<Result<tool::Output>> forget_memory(memory::longterm::Backend& backend,
                                                                   std::string scope_key,
                                                                   tool::MemoryForgetRequest request,
                                                                   tool::DispatchContext& ctx) {
  auto key = memory::longterm::RecordKey{.id = std::move(request.id), .scope_key = scope_key};
  const auto started_at = core::time::now_utc();
  auto removed = co_await asio::co_spawn(ctx.executor, backend.remove(key), asio::use_awaitable);
  if (!removed) {
    co_return std::unexpected(std::move(removed).error());
  }
  if (ctx.bus != nullptr) {
    const auto finished_at = core::time::now_utc();
    co_await ctx.bus->publish_advisory(hook::Event::memory_forget,
                                       make_memory_forget_payload(key, ctx, started_at, finished_at));
  }
  auto data_json = memory::longterm::render_forget_data_json(key);
  co_return tool::Output{
      .text = render_memory_forget_tool_text(key),
      .data_json = std::move(data_json),
      .attachments = {},
      .usage =
          tool::ToolUsage{
              .bytes_written = 0,
          },
      .is_error = false,
  };
}

}  // namespace

void bind_memory_tools(tool::DispatchContext& context,
                       memory::longterm::Backend& backend,
                       std::string scope_key) {
  context.memory_recall = [&backend, scope_key](tool::MemoryRecallRequest request, tool::DispatchContext& ctx) {
    return recall_memory(backend, scope_key, std::move(request), ctx);
  };
  context.memory_remember = [&backend, scope_key](tool::MemoryRememberRequest request, tool::DispatchContext& ctx) {
    return remember_memory(backend, scope_key, std::move(request), ctx);
  };
  context.memory_forget = [&backend, scope_key](tool::MemoryForgetRequest request, tool::DispatchContext& ctx) {
    return forget_memory(backend, scope_key, std::move(request), ctx);
  };
}

}  // namespace orangutan::bootstrap
