#include <oran/memory/longterm.hpp>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/core/time.hpp>

namespace orangutan::memory::longterm {

async::Awaitable<core::Result<RecallResult>> recall(Backend& backend, RecallRequest request) {
  if (auto valid = validate_recall_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  std::vector<SearchHit> hits;
  if (!request.record_id.empty()) {
    auto record =
        co_await backend.get(RecordKey{.id = std::move(request.record_id), .scope_key = request.query.scope_key});
    if (!record) {
      co_return std::unexpected(std::move(record).error());
    }
    if (record->shadow ||
        (!request.query.kinds.empty() && !std::ranges::contains(request.query.kinds, record->kind))) {
      co_return std::unexpected(core::Error::not_found("long-term memory record not found"));
    }
    hits.push_back(SearchHit{.record = std::move(*record)});
  } else {
    auto found = co_await backend.search(std::move(request.query), request.limit);
    if (!found) {
      co_return std::unexpected(std::move(found).error());
    }
    hits = std::move(*found);
  }

  if (!hits.empty()) {
    // Stored timestamps have millisecond precision; the snapshot reports the persisted value.
    const auto read_at =
        core::Time{std::chrono::floor<std::chrono::milliseconds>(core::time::now_utc().to_system_time_point())};
    auto keys = std::vector<RecordKey>{};
    keys.reserve(hits.size());
    for (const auto& hit : hits) {
      keys.push_back(hit.record.key);
    }
    if (auto touched = co_await backend.touch(std::move(keys), read_at); !touched) {
      co_return std::unexpected(std::move(touched).error());
    }
    // The returned content is the selected snapshot; only its read time advances.
    for (auto& hit : hits) {
      hit.record.last_read_at = std::max(hit.record.last_read_at, read_at);
    }
  }

  auto text = render_recall_text(hits);
  co_return RecallResult{
      .hits = std::move(hits),
      .text = std::move(text),
  };
}

async::Awaitable<core::Result<IndexResult>> index(Backend& backend, IndexRequest request) {
  if (auto valid = validate_index_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }
  auto entries = co_await backend.list(request);
  if (!entries) {
    co_return std::unexpected(std::move(entries).error());
  }
  co_return make_index(*entries, request);
}

}  // namespace orangutan::memory::longterm
