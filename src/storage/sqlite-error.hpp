#pragma once

#include "types/base.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace orangutan::sqlite {

    /// Categorizes the failure mode of a sqlite operation.
    enum class sqlite_error_kind : base::u8 {
        /// Failed to open or configure the database handle.
        open_failed,
        /// sqlite3_prepare_v2 returned non-OK, produced no statement, or left trailing SQL.
        prepare_failed,
        /// sqlite3_step / sqlite3_exec returned an unexpected code.
        step_failed,
        /// sqlite3_bind_* returned non-OK.
        bind_failed,
        /// sqlite3_clear_bindings / sqlite3_reset returned non-OK.
        reset_failed,
        /// A column accessor, row mapping, or serialization produced an invalid value.
        mapping_error,
        /// Statement lifecycle misuse: already consumed, already bound, or not yet executed.
        state_error,
        /// Input violates a pre-condition: out-of-range index, mixed placeholders, arg-count mismatch.
        argument_error,
        /// Row count does not match the caller's expectation (one / at-most-one).
        row_count_mismatch,
    };

    /// Failure payload returned from every fallible sqlite operation.
    struct SqliteError {
        sqlite_error_kind kind{};
        std::string message;
        /// Raw sqlite3 return code when applicable; 0 for non-sqlite failures.
        int rc = 0;

        [[nodiscard]]
        auto to_string() const -> std::string {
            return message;
        }
    };

    /// Convenience alias: every value-returning sqlite API produces this.
    template <typename T>
    using SqliteResult = std::expected<T, SqliteError>;

    /// Produce an error without a raw sqlite rc (argument / state failures).
    inline auto make_error(sqlite_error_kind kind, std::string message) -> SqliteError {
        return SqliteError{.kind = kind, .message = std::move(message), .rc = 0};
    }

    /// Produce an error carrying a raw sqlite rc.
    inline auto make_error(sqlite_error_kind kind, std::string message, int rc) -> SqliteError {
        return SqliteError{.kind = kind, .message = std::move(message), .rc = rc};
    }

    /// Shorthand for unexpected(SqliteError) at call sites.
    inline auto fail(sqlite_error_kind kind, std::string message) -> std::unexpected<SqliteError> {
        return std::unexpected<SqliteError>{make_error(kind, std::move(message))};
    }

    inline auto fail(sqlite_error_kind kind, std::string message, int rc) -> std::unexpected<SqliteError> {
        return std::unexpected<SqliteError>{make_error(kind, std::move(message), rc)};
    }

    /// Assemble the canonical "prefix: sqlite3_errmsg" string without throwing.
    /// Kept as a free function so it can be used both here and by future wrappers
    /// inside sqlite.hpp's detail namespace.
    [[nodiscard]]
    inline auto format_sqlite_message(std::string_view prefix, std::string_view errmsg) -> std::string {
        std::string out;
        out.reserve(prefix.size() + 2 + errmsg.size());
        out.append(prefix);
        out.append(": ");
        out.append(errmsg);
        return out;
    }

} // namespace orangutan::sqlite
