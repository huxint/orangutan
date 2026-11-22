// include/oran/storage/sqlite.hpp — expected-only SQLite core.

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::storage {

enum class OpenMode : std::uint8_t {
  read_only,
  read_write,
  read_write_create,
};

struct ConnectionOptions {
  std::string path;
  OpenMode mode{OpenMode::read_write_create};
  int busy_timeout_ms{5000};
  bool enable_wal{true};
  bool enforce_foreign_keys{true};
};

enum class StepResult : std::uint8_t {
  row,
  done,
};

/// Positional SQL NULL for `Statement::bind_all`.
struct Null {};

using ColumnValue = std::optional<std::string>;
using BlobValue = std::optional<std::vector<std::byte>>;

struct Row {
  std::vector<ColumnValue> values;
};

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<Row> rows;
};

class Statement {
public:
  Statement();
  ~Statement();

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) noexcept;
  Statement& operator=(Statement&&) noexcept;

  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] core::Result<void> bind_null(int index);
  [[nodiscard]] core::Result<void> bind_int64(int index, std::int64_t value);
  [[nodiscard]] core::Result<void> bind_double(int index, double value);
  [[nodiscard]] core::Result<void> bind_text(int index, std::string_view value);
  [[nodiscard]] core::Result<void> bind_blob(int index, std::span<const std::byte> value);
  /// Bind values to parameters 1..N in order, stopping at the first failure.
  /// Accepts text, byte spans (blobs), integers, bools, doubles, `Null` and
  /// optionals of those.
  template <class... Values>
  [[nodiscard]] core::Result<void> bind_all(const Values&... values) {
    int index = 0;
    core::Result<void> bound{};
    [[maybe_unused]] const bool ok = ((bound = bind_value(++index, values)).has_value() && ...);
    return bound;
  }

  [[nodiscard]] core::Result<void> clear_bindings();
  [[nodiscard]] core::Result<void> reset();

  [[nodiscard]] core::Result<StepResult> step();
  /// Step once more and require that the statement has no further rows.
  [[nodiscard]] core::Result<void> expect_done(std::string_view operation);

  [[nodiscard]] core::Result<int> column_count() const;
  [[nodiscard]] core::Result<std::string> column_name(int index) const;
  [[nodiscard]] core::Result<ColumnValue> column_text(int index) const;
  /// Text that must not be NULL; errors name `field`.
  [[nodiscard]] core::Result<std::string> required_text(int index, std::string_view field) const;
  [[nodiscard]] core::Result<BlobValue> column_blob(int index) const;
  [[nodiscard]] core::Result<std::int64_t> column_int64(int index) const;
  [[nodiscard]] core::Result<double> column_double(int index) const;

private:
  template <class T>
  [[nodiscard]] core::Result<void> bind_value(int index, const T& value) {
    if constexpr (std::same_as<T, Null>) {
      return bind_null(index);
    } else if constexpr (requires {
                           value.has_value();
                           *value;
                         }) {
      return value.has_value() ? bind_value(index, *value) : bind_null(index);
    } else if constexpr (std::same_as<T, bool>) {
      return bind_int64(index, value ? 1 : 0);
    } else if constexpr (std::integral<T>) {
      return bind_int64(index, static_cast<std::int64_t>(value));
    } else if constexpr (std::floating_point<T>) {
      return bind_double(index, static_cast<double>(value));
    } else if constexpr (std::convertible_to<const T&, std::span<const std::byte>>) {
      return bind_blob(index, std::span<const std::byte>{value});
    } else {
      static_assert(std::convertible_to<const T&, std::string_view>, "unsupported SQLite parameter type");
      return bind_text(index, std::string_view{value});
    }
  }

  struct Impl;
  explicit Statement(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;

  friend class Connection;
};

class Connection {
public:
  Connection();
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) noexcept;
  Connection& operator=(Connection&&) noexcept;

  [[nodiscard]] static core::Result<Connection> open(ConnectionOptions options);

  [[nodiscard]] bool is_open() const noexcept;
  void close() noexcept;

  /// Consistent SQLite snapshot, including committed WAL data. Destination must
  /// not exist. Host owns destination directory authority and privacy.
  [[nodiscard]] core::Result<void> backup_to(std::string_view destination);
  [[nodiscard]] core::Result<void> execute(std::string_view sql);
  [[nodiscard]] core::Result<Statement> prepare(std::string_view sql);
  [[nodiscard]] core::Result<QueryResult> query(std::string_view sql);

private:
  struct Impl;
  explicit Connection(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

/// An IMMEDIATE transaction on a borrowed connection. It rolls back on
/// destruction unless `commit` succeeded; the connection must outlive it.
class Transaction {
public:
  [[nodiscard]] static core::Result<Transaction> begin(Connection& connection);

  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&& other) noexcept;
  Transaction& operator=(Transaction&&) = delete;

  [[nodiscard]] core::Result<void> commit();

private:
  explicit Transaction(Connection& connection) noexcept : connection_{&connection} {}

  Connection* connection_{};
};

}  // namespace orangutan::storage
