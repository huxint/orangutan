#pragma once

#include "storage/sqlite.hpp"

#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace orangutan::sqlite {

    /// Translate a `SqliteResult<T>` into the underlying value, throwing
    /// `std::runtime_error` on error. Consumer modules whose public API still
    /// propagates failures via exceptions (e.g. SessionStore, MemoryStore) use
    /// this at the sqlite boundary so that internal expected-returning code can
    /// stay explicit while the outer signature remains unchanged.
    template <typename T>
        requires (!std::is_void_v<T>)
    auto unwrap(SqliteResult<T> result) -> T {
        if (!result) {
            throw std::runtime_error("sqlite: " + result.error().message);
        }
        return std::move(*result);
    }

    inline void unwrap(SqliteResult<void> result) {
        if (!result) {
            throw std::runtime_error("sqlite: " + result.error().message);
        }
    }

    /// Open (or prepare + bind + run + unwrap) a one-shot command.
    template <typename... Args>
    void exec_bind(const Database &db, std::string_view sql, Args &&...args) {
        auto command = unwrap(db.exec(sql));
        unwrap(command.bind(std::forward<Args>(args)...).run());
    }

    /// exec_script + unwrap.
    inline void exec_script(const Database &db, std::string_view sql, std::string_view context = {}) {
        unwrap(db.exec_script(sql, context));
    }

    /// Prepare + bind + one<T> + unwrap.
    template <typename T, typename... Args>
    [[nodiscard]]
    auto query_one(const Database &db, std::string_view sql, Args &&...args) -> T {
        auto query = unwrap(db.query(sql));
        return unwrap(query.bind(std::forward<Args>(args)...).template one<T>());
    }

    /// Prepare + bind + optional<T> + unwrap.
    template <typename T, typename... Args>
    [[nodiscard]]
    auto query_optional(const Database &db, std::string_view sql, Args &&...args) -> std::optional<T> {
        auto query = unwrap(db.query(sql));
        return unwrap(query.bind(std::forward<Args>(args)...).template optional<T>());
    }

    /// Prepare + bind + all<T> + unwrap.
    template <typename T, typename... Args>
    [[nodiscard]]
    auto query_all(const Database &db, std::string_view sql, Args &&...args) -> std::vector<T> {
        auto query = unwrap(db.query(sql));
        return unwrap(query.bind(std::forward<Args>(args)...).template all<T>());
    }

    /// Open a database, unwrapping the factory expected into a value or exception.
    [[nodiscard]]
    inline auto open_or_throw(const std::filesystem::path &path) -> Database {
        return unwrap(Database::create(path));
    }

    /// Prepare a statement from sql, throwing on error.
    [[nodiscard]]
    inline auto prepare_or_throw(const Database &db, std::string_view sql) -> Statement {
        return unwrap(Statement::create(db, sql));
    }

    /// Begin a transaction, throwing on error.
    [[nodiscard]]
    inline auto begin_or_throw(Database &db) -> Transaction {
        return unwrap(Transaction::create(db));
    }

} // namespace orangutan::sqlite
