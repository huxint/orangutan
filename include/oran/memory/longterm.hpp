// include/oran/memory/longterm.hpp — long-term memory backend contracts.

#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/storage/migrations.hpp>

namespace orangutan::storage {
class Pool;
}  // namespace orangutan::storage

namespace orangutan::memory::longterm {

/// Stable record kind for persistent memory rows.
enum class RecordKind : std::uint8_t {
  user,
  feedback,
  project,
  reference,
  team,
};

struct RecordKey {
  std::string id;
  std::string scope_key;

  friend bool operator==(const RecordKey&, const RecordKey&) = default;
};

struct Record {
  RecordKey key;
  RecordKind kind{RecordKind::project};
  std::string title;
  std::string body;
  core::Time created_at{core::Time::epoch()};
  core::Time updated_at{core::Time::epoch()};
  core::Time last_read_at{core::Time::epoch()};
  double importance{0.0};
  std::vector<std::string> tags;
  std::vector<std::string> linked_record_ids;
  bool shadow{false};

  friend bool operator==(const Record&, const Record&) = default;
};

struct Query {
  std::string scope_key;
  std::string text;
  std::vector<RecordKind> kinds;

  friend bool operator==(const Query&, const Query&) = default;
};

struct SearchHit {
  Record record;
  double score{0.0};

  friend bool operator==(const SearchHit&, const SearchHit&) = default;
};

struct RecallRequest {
  Query query;
  std::size_t limit{0};
  /// Exact read within query.scope_key; mutually exclusive with query.text.
  std::string record_id{};

  friend bool operator==(const RecallRequest&, const RecallRequest&) = default;
};

struct RecallResult {
  std::vector<SearchHit> hits;
  std::string text;

  friend bool operator==(const RecallResult&, const RecallResult&) = default;
};

inline constexpr std::size_t kMaxIndexBytes = 8192;

struct IndexRequest {
  std::string scope_key{};
  std::vector<RecordKind> kinds{};
  std::size_t limit{20};
  std::size_t offset{0};
  std::size_t max_bytes{kMaxIndexBytes};
};

/// A discovery cue, not a copy of the note. Existing records need no migration.
struct IndexEntry {
  RecordKey key{};
  RecordKind kind{RecordKind::project};
  std::string title{};
  std::string summary{};

  friend bool operator==(const IndexEntry&, const IndexEntry&) = default;
};

struct IndexResult {
  std::vector<IndexEntry> entries{};
  std::string text{};
  std::optional<std::size_t> next_offset{};
  /// Legacy IDs too large to fit even one entry are reported and skipped.
  std::size_t omitted_count{0};

  friend bool operator==(const IndexResult&, const IndexResult&) = default;
};

/// Scoped storage effects; implementations validate inputs before accessing storage.
class Backend {
public:
  Backend() = default;
  virtual ~Backend() = default;

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  Backend(Backend&&) = delete;
  Backend& operator=(Backend&&) = delete;

  [[nodiscard]] virtual async::Awaitable<core::Result<Record>> get(RecordKey key) = 0;
  /// Full notes matching topic words, excluding shadow rows.
  [[nodiscard]] virtual async::Awaitable<core::Result<std::vector<SearchHit>>> search(Query query,
                                                                                      std::size_t limit) = 0;
  /// Return bounded cues and at most one look-ahead entry, without returning
  /// full bodies or acquiring a writer.
  [[nodiscard]] virtual async::Awaitable<core::Result<std::vector<IndexEntry>>> list(IndexRequest request) = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<Record>> upsert(Record record) = 0;
  /// Advance read timestamps in one write; rows removed meanwhile are skipped.
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> touch(std::vector<RecordKey> keys, core::Time read_at) = 0;
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> remove(RecordKey key) = 0;
};

struct Fts5BackendOptions {
  std::string migrations_directory;

  friend bool operator==(const Fts5BackendOptions&, const Fts5BackendOptions&) = default;
};

/// Scoped lexical records and their transactional FTS5 index. The pool is borrowed.
class Fts5Backend final : public Backend {
public:
  explicit Fts5Backend(storage::Pool& pool, Fts5BackendOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<storage::MigrationReport>> migrate();

  /// Host maintenance on the caller's blocking executor. Backup requires a new
  /// private path; import copies one explicit scope into an empty destination.
  [[nodiscard]] async::Awaitable<core::Result<void>> backup_to(std::string destination);
  [[nodiscard]] async::Awaitable<core::Result<void>>
  import_scope(std::string source_path, std::string source_scope, std::string destination_scope);

  [[nodiscard]] async::Awaitable<core::Result<Record>> get(RecordKey key) override;
  [[nodiscard]] async::Awaitable<core::Result<std::vector<SearchHit>>> search(Query query, std::size_t limit) override;
  [[nodiscard]] async::Awaitable<core::Result<std::vector<IndexEntry>>> list(IndexRequest request) override;
  [[nodiscard]] async::Awaitable<core::Result<Record>> upsert(Record record) override;
  [[nodiscard]] async::Awaitable<core::Result<void>> touch(std::vector<RecordKey> keys, core::Time read_at) override;
  [[nodiscard]] async::Awaitable<core::Result<void>> remove(RecordKey key) override;

private:
  storage::Pool* pool_{};
  Fts5BackendOptions options_;
};

/// Read an exact ID or search topic words, then update read timestamps and render.
/// The backend must outlive the awaited operation.
[[nodiscard]] async::Awaitable<core::Result<RecallResult>> recall(Backend& backend, RecallRequest request);

/// Browse bounded metadata without touching read timestamps or returning bodies.
[[nodiscard]] async::Awaitable<core::Result<IndexResult>> index(Backend& backend, IndexRequest request);

/// Pure projection and budgeting. Candidates include one look-ahead row for paging.
[[nodiscard]] core::Result<IndexResult> make_index(std::span<const IndexEntry> candidates, const IndexRequest& request);

[[nodiscard]] std::string render_recall_text(std::span<const SearchHit> hits);
[[nodiscard]] std::string render_index_data_json(const IndexResult& index);
[[nodiscard]] std::string render_recall_data_json(std::span<const SearchHit> hits);
[[nodiscard]] std::string render_remember_data_json(const Record& record);
[[nodiscard]] std::string render_forget_data_json(const RecordKey& key);
[[nodiscard]] core::Result<void> validate_key(const RecordKey& key);
[[nodiscard]] core::Result<void> validate_record(const Record& record);
[[nodiscard]] core::Result<void> validate_query(const Query& query, std::size_t limit);
[[nodiscard]] core::Result<void> validate_recall_request(const RecallRequest& request);
[[nodiscard]] core::Result<void> validate_index_request(const IndexRequest& request);

}  // namespace orangutan::memory::longterm

template <>
struct std::formatter<orangutan::memory::longterm::RecordKind> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::memory::longterm::RecordKind kind, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(kind), ctx);
  }
};
