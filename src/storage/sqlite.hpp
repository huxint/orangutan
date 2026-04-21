#pragma once

#include <algorithm>
#include <cctype>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "storage/sqlite-error.hpp"

namespace orangutan::sqlite {

    class Database;
    class Statement;
    class Transaction;
    class Command;
    class Query;
    class Row;

    template <typename T>
    struct RowMapper;

    namespace detail {

        constexpr auto DEFAULT_BUSY_TIMEOUT_MS = 1000;

        enum class placeholder_mode : std::uint8_t {
            none,
            anonymous,
            indexed,
            mixed,
        };

        struct PlaceholderInfo {
            placeholder_mode mode = placeholder_mode::none;
            int expected_bind_count = 0;
            bool indexed_contiguous = true;
        };

        template <typename T>
        concept tuple_like = requires {
            typename std::tuple_size<std::remove_cvref_t<T>>::type;
        };

        /// Canonical "prefix: sqlite3_errmsg" message without throwing.
        [[nodiscard]]
        inline auto format_sqlite_error(sqlite3 *db, std::string_view prefix) -> std::string {
            const auto *errmsg = db != nullptr ? sqlite3_errmsg(db) : nullptr;
            const auto tail = errmsg != nullptr ? std::string_view{errmsg} : std::string_view{"unknown error"};
            return format_sqlite_message(prefix, tail);
        }

        [[nodiscard]]
        inline auto check_sqlite_ok(int rc, sqlite3 *db, std::string_view context) -> SqliteResult<void> {
            if (rc == SQLITE_OK) {
                return {};
            }
            return fail(sqlite_error_kind::step_failed, format_sqlite_error(db, context), rc);
        }

        [[nodiscard]]
        inline auto check_bind_ok(int rc, sqlite3_stmt *stmt) -> SqliteResult<void> {
            if (rc == SQLITE_OK) {
                return {};
            }
            return fail(sqlite_error_kind::bind_failed, format_sqlite_error(sqlite3_db_handle(stmt), "sqlite bind failed"), rc);
        }

        [[nodiscard]]
        inline auto is_space(char ch) -> bool {
            return std::isspace(static_cast<unsigned char>(ch)) != 0;
        }

        [[nodiscard]]
        inline auto prepare_statement(sqlite3 *db, std::string_view sql) -> SqliteResult<sqlite3_stmt *> {
            auto sql_text = std::string(sql);
            sqlite3_stmt *stmt = nullptr;
            const char *tail = nullptr;
            const auto rc = sqlite3_prepare_v2(db, sql_text.c_str(), static_cast<int>(sql_text.size()), &stmt, &tail);
            if (rc != SQLITE_OK) {
                return fail(sqlite_error_kind::prepare_failed, format_sqlite_error(db, "sqlite prepare failed"), rc);
            }
            if (stmt == nullptr) {
                return fail(sqlite_error_kind::prepare_failed, "sqlite prepare failed: empty sql");
            }
            while (tail != nullptr && *tail != '\0' && is_space(*tail)) {
                ++tail;
            }
            if (tail != nullptr && *tail != '\0') {
                sqlite3_finalize(stmt);
                return fail(sqlite_error_kind::prepare_failed, "sqlite prepare failed: trailing sql");
            }
            return stmt;
        }

        [[nodiscard]]
        inline auto analyze_placeholders(std::string_view sql) -> PlaceholderInfo {
            auto info = PlaceholderInfo{};
            std::vector<int> indexed_ids;
            int anonymous_count = 0;

            enum class parse_state : std::uint8_t {
                normal,
                single_quote,
                double_quote,
                bracket_identifier,
                line_comment,
                block_comment,
            };

            auto state = parse_state::normal;
            for (std::size_t index = 0; index < sql.size(); ++index) {
                const auto ch = sql[index];
                switch (state) {
                    case parse_state::normal:
                        if (ch == '\'') {
                            state = parse_state::single_quote;
                            continue;
                        }
                        if (ch == '"') {
                            state = parse_state::double_quote;
                            continue;
                        }
                        if (ch == '[') {
                            state = parse_state::bracket_identifier;
                            continue;
                        }
                        if (ch == '-' && index + 1 < sql.size() && sql[index + 1] == '-') {
                            state = parse_state::line_comment;
                            ++index;
                            continue;
                        }
                        if (ch == '/' && index + 1 < sql.size() && sql[index + 1] == '*') {
                            state = parse_state::block_comment;
                            ++index;
                            continue;
                        }
                        if (ch != '?') {
                            continue;
                        }

                        {
                            auto lookahead = index + 1;
                            while (lookahead < sql.size() && std::isdigit(static_cast<unsigned char>(sql[lookahead])) != 0) {
                                ++lookahead;
                            }

                            if (lookahead == index + 1) {
                                ++anonymous_count;
                            } else {
                                indexed_ids.push_back(std::stoi(std::string(sql.substr(index + 1, lookahead - index - 1))));
                            }
                            index = lookahead - 1;
                        }
                        break;

                    case parse_state::single_quote:
                        if (ch == '\'') {
                            if (index + 1 < sql.size() && sql[index + 1] == '\'') {
                                ++index;
                            } else {
                                state = parse_state::normal;
                            }
                        }
                        break;

                    case parse_state::double_quote:
                        if (ch == '"') {
                            if (index + 1 < sql.size() && sql[index + 1] == '"') {
                                ++index;
                            } else {
                                state = parse_state::normal;
                            }
                        }
                        break;

                    case parse_state::bracket_identifier:
                        if (ch == ']') {
                            state = parse_state::normal;
                        }
                        break;

                    case parse_state::line_comment:
                        if (ch == '\n') {
                            state = parse_state::normal;
                        }
                        break;

                    case parse_state::block_comment:
                        if (ch == '*' && index + 1 < sql.size() && sql[index + 1] == '/') {
                            state = parse_state::normal;
                            ++index;
                        }
                        break;
                }
            }

            if (anonymous_count > 0 && !indexed_ids.empty()) {
                info.mode = placeholder_mode::mixed;
            } else if (anonymous_count > 0) {
                info.mode = placeholder_mode::anonymous;
                info.expected_bind_count = anonymous_count;
            } else if (!indexed_ids.empty()) {
                info.mode = placeholder_mode::indexed;
                std::ranges::sort(indexed_ids);
                indexed_ids.erase(std::ranges::unique(indexed_ids).begin(), indexed_ids.end());
                info.expected_bind_count = indexed_ids.back();
                for (int expected = 1; expected <= info.expected_bind_count; ++expected) {
                    if (!std::ranges::binary_search(indexed_ids, expected)) {
                        info.indexed_contiguous = false;
                        break;
                    }
                }
            }

            return info;
        }

        [[nodiscard]]
        inline auto bind_count_message(std::size_t expected, std::size_t actual) -> std::string {
            return "bind parameter count mismatch: expected " + std::to_string(expected) + ", got " + std::to_string(actual);
        }

        [[nodiscard]]
        inline auto column_count_message(std::size_t expected, int actual) -> std::string {
            if (expected == 1U) {
                return "expected one column, got " + std::to_string(actual);
            }
            return "expected " + std::to_string(expected) + " columns, got " + std::to_string(actual);
        }

        [[nodiscard]]
        inline auto validate_column_index(sqlite3_stmt *stmt, int index) -> SqliteResult<void> {
            if (index < 0 || index >= sqlite3_column_count(stmt)) {
                return fail(sqlite_error_kind::argument_error, "column index out of range");
            }
            return {};
        }

        [[nodiscard]]
        inline auto copy_column_text(sqlite3_stmt *stmt, int index) -> std::string {
            const auto *text = sqlite3_column_text(stmt, index);
            if (text == nullptr) {
                return {};
            }
            const auto size = static_cast<std::size_t>(sqlite3_column_bytes(stmt, index));
            return {reinterpret_cast<const char *>(text), size}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        }

        template <typename T>
        [[nodiscard]]
        auto bind_value(sqlite3_stmt *stmt, int index, T &&value) -> SqliteResult<void> {
            using value_type = std::remove_cvref_t<T>;

            if constexpr (requires {
                              typename value_type::value_type;
                              requires std::same_as<value_type, std::optional<typename value_type::value_type>>;
                          }) {
                if (value.has_value()) {
                    return bind_value(stmt, index, *std::forward<T>(value));
                }
                return check_bind_ok(sqlite3_bind_null(stmt, index), stmt);
            } else if constexpr (std::same_as<value_type, std::nullptr_t>) {
                return check_bind_ok(sqlite3_bind_null(stmt, index), stmt);
            } else if constexpr (std::same_as<value_type, bool>) {
                return check_bind_ok(sqlite3_bind_int(stmt, index, value ? 1 : 0), stmt);
            } else if constexpr (std::same_as<value_type, int>) {
                return check_bind_ok(sqlite3_bind_int(stmt, index, value), stmt);
            } else if constexpr (std::same_as<value_type, std::int64_t>) {
                return check_bind_ok(sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(value)), stmt);
            } else if constexpr (std::same_as<value_type, double>) {
                return check_bind_ok(sqlite3_bind_double(stmt, index, value), stmt);
            } else if constexpr (std::is_pointer_v<value_type> && std::same_as<std::remove_cv_t<std::remove_pointer_t<value_type>>, char>) {
                if (value == nullptr) {
                    return check_bind_ok(sqlite3_bind_null(stmt, index), stmt);
                }
                return check_bind_ok(sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT), stmt);
            } else if constexpr (std::is_convertible_v<T, std::string_view>) {
                const auto text_value = [&value]() -> std::string_view {
                    if constexpr (std::is_array_v<value_type>) {
                        return {std::data(value)};
                    } else {
                        return {value};
                    }
                }();
                const char *text = text_value.data();
                if (text == nullptr) {
                    text = "";
                }
                return check_bind_ok(sqlite3_bind_text(stmt, index, text, static_cast<int>(text_value.size()), SQLITE_TRANSIENT), stmt);
            } else {
                static_assert(false, "unsupported sqlite bind type");
            }
        }

        template <typename T>
        [[nodiscard]]
        auto column_value(sqlite3_stmt *stmt, int index) -> SqliteResult<T> {
            if (auto check = validate_column_index(stmt, index); !check) {
                return std::unexpected(check.error());
            }
            using value_type = std::remove_cvref_t<T>;

            if constexpr (requires {
                              typename value_type::value_type;
                              requires std::same_as<value_type, std::optional<typename value_type::value_type>>;
                          }) {
                if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
                    return T{std::nullopt};
                }
                auto inner = column_value<typename value_type::value_type>(stmt, index);
                if (!inner) {
                    return std::unexpected(inner.error());
                }
                return T{std::move(*inner)};
            } else {
                if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
                    return fail(sqlite_error_kind::mapping_error, "column " + std::to_string(index) + " is null");
                }
                if constexpr (std::same_as<value_type, std::string>) {
                    return copy_column_text(stmt, index);
                } else if constexpr (std::same_as<value_type, int>) {
                    return sqlite3_column_int(stmt, index);
                } else if constexpr (std::same_as<value_type, std::int64_t>) {
                    return static_cast<value_type>(sqlite3_column_int64(stmt, index));
                } else if constexpr (std::same_as<value_type, double>) {
                    return sqlite3_column_double(stmt, index);
                } else if constexpr (std::same_as<value_type, bool>) {
                    return sqlite3_column_int(stmt, index) != 0;
                } else {
                    static_assert(false, "unsupported sqlite column type");
                }
            }
        }

        template <typename T>
        auto map_row(const Row &row) -> SqliteResult<T>;

    } // namespace detail

    class Row {
    public:
        template <typename T>
        [[nodiscard]]
        auto get(int index) const -> SqliteResult<T> {
            return detail::column_value<T>(stmt_, index);
        }

        [[nodiscard]]
        auto is_null(int index) const -> SqliteResult<bool> {
            if (auto check = detail::validate_column_index(stmt_, index); !check) {
                return std::unexpected(check.error());
            }
            return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
        }

        [[nodiscard]]
        auto columns() const noexcept -> int {
            return sqlite3_column_count(stmt_);
        }

    private:
        explicit Row(sqlite3_stmt *stmt) noexcept
        : stmt_(stmt) {}

        sqlite3_stmt *stmt_ = nullptr;

        friend class Statement;
    };

    class Statement {
    public:
        [[nodiscard]] static auto create(const Database &db, std::string_view sql) -> SqliteResult<Statement>;

        ~Statement() {
            finalize();
        }

        Statement(const Statement &) = delete;
        auto operator=(const Statement &) -> Statement & = delete;

        Statement(Statement &&other) noexcept
        : stmt_(std::exchange(other.stmt_, nullptr)),
          placeholder_info_(other.placeholder_info_),
          has_row_(std::exchange(other.has_row_, false)),
          pending_error_(std::move(other.pending_error_)) {
            other.pending_error_.reset();
        }

        auto operator=(Statement &&other) noexcept -> Statement & {
            if (this != &other) {
                finalize();
                stmt_ = std::exchange(other.stmt_, nullptr);
                placeholder_info_ = other.placeholder_info_;
                has_row_ = std::exchange(other.has_row_, false);
                pending_error_ = std::move(other.pending_error_);
                other.pending_error_.reset();
            }
            return *this;
        }

        /// Fluent bind: latches the first error and returns *this. The latched
        /// error surfaces at step()/row()/reset().
        template <typename T>
        auto bind(int index, T &&value) -> Statement & {
            if (pending_error_.has_value()) {
                return *this;
            }
            if (auto result = detail::bind_value(stmt_, index, std::forward<T>(value)); !result) {
                pending_error_ = result.error();
            }
            return *this;
        }

        template <typename... Args>
        auto bind_all(Args &&...args) -> Statement & {
            if (pending_error_.has_value()) {
                return *this;
            }
            if (auto check = try_validate_bind_count(sizeof...(Args)); !check) {
                pending_error_ = check.error();
                return *this;
            }
            int parameter_index = 1;
            (bind(parameter_index++, std::forward<Args>(args)), ...);
            return *this;
        }

        template <typename T>
        [[nodiscard]]
        auto try_bind(int index, T &&value) -> SqliteResult<void> {
            if (pending_error_.has_value()) {
                auto err = std::move(*pending_error_);
                pending_error_.reset();
                return std::unexpected(std::move(err));
            }
            return detail::bind_value(stmt_, index, std::forward<T>(value));
        }

        template <typename... Args>
        [[nodiscard]]
        auto try_bind_all(Args &&...args) -> SqliteResult<void> {
            if (pending_error_.has_value()) {
                auto err = std::move(*pending_error_);
                pending_error_.reset();
                return std::unexpected(std::move(err));
            }
            if (auto check = try_validate_bind_count(sizeof...(Args)); !check) {
                return check;
            }
            SqliteResult<void> final_result{};
            int parameter_index = 1;
            (([&](auto &&arg) {
                if (!final_result) {
                    return;
                }
                if (auto r = detail::bind_value(stmt_, parameter_index, std::forward<decltype(arg)>(arg)); !r) {
                    final_result = std::unexpected(r.error());
                }
                ++parameter_index;
            }(std::forward<Args>(args))),
             ...);
            return final_result;
        }

        [[nodiscard]]
        auto step() -> SqliteResult<bool> {
            if (pending_error_.has_value()) {
                auto err = std::move(*pending_error_);
                pending_error_.reset();
                return std::unexpected(std::move(err));
            }
            const auto rc = sqlite3_step(stmt_);
            if (rc == SQLITE_ROW) {
                has_row_ = true;
                return true;
            }
            if (rc == SQLITE_DONE) {
                has_row_ = false;
                return false;
            }
            return fail(sqlite_error_kind::step_failed, detail::format_sqlite_error(sqlite3_db_handle(stmt_), "sqlite step failed"), rc);
        }

        [[nodiscard]]
        auto row() const -> SqliteResult<Row> {
            if (pending_error_.has_value()) {
                return std::unexpected(*pending_error_);
            }
            if (!has_row_) {
                return fail(sqlite_error_kind::state_error, "statement does not point at a row");
            }
            return Row(stmt_);
        }

        [[nodiscard]]
        auto reset() -> SqliteResult<void> {
            pending_error_.reset();
            has_row_ = false;
            const auto rc = sqlite3_reset(stmt_);
            if (rc != SQLITE_OK) {
                return fail(sqlite_error_kind::reset_failed, detail::format_sqlite_error(sqlite3_db_handle(stmt_), "sqlite reset failed"), rc);
            }
            return {};
        }

        [[nodiscard]]
        auto clear_bindings() -> SqliteResult<void> {
            const auto rc = sqlite3_clear_bindings(stmt_);
            if (rc != SQLITE_OK) {
                return fail(sqlite_error_kind::reset_failed, detail::format_sqlite_error(sqlite3_db_handle(stmt_), "sqlite clear bindings failed"), rc);
            }
            return {};
        }

        /// Reports any deferred error accumulated by fluent `bind()` calls.
        /// Consumed (cleared) by this call.
        [[nodiscard]]
        auto take_pending_error() -> std::optional<SqliteError> {
            auto err = std::move(pending_error_);
            pending_error_.reset();
            return err;
        }

    private:
        Statement(sqlite3_stmt *stmt, detail::PlaceholderInfo info) noexcept
        : stmt_(stmt),
          placeholder_info_(info) {}

        void finalize() noexcept {
            if (stmt_ != nullptr) {
                sqlite3_finalize(stmt_);
                stmt_ = nullptr;
            }
        }

        [[nodiscard]]
        auto try_validate_bind_count(std::size_t actual) const -> SqliteResult<void> {
            if (placeholder_info_.mode == detail::placeholder_mode::mixed) {
                return fail(sqlite_error_kind::argument_error, "mixed placeholder styles are not supported");
            }
            if (placeholder_info_.mode == detail::placeholder_mode::indexed && !placeholder_info_.indexed_contiguous) {
                return fail(sqlite_error_kind::argument_error, "indexed placeholders must be contiguous");
            }
            if (std::cmp_not_equal(actual, placeholder_info_.expected_bind_count)) {
                return fail(sqlite_error_kind::argument_error,
                            detail::bind_count_message(static_cast<std::size_t>(placeholder_info_.expected_bind_count), actual));
            }
            return {};
        }

        [[nodiscard]]
        auto result_columns() const noexcept -> int {
            return sqlite3_column_count(stmt_);
        }

        [[nodiscard]]
        auto expected_bind_count() const noexcept -> int {
            return placeholder_info_.expected_bind_count;
        }

        sqlite3_stmt *stmt_ = nullptr;
        detail::PlaceholderInfo placeholder_info_{};
        bool has_row_ = false;
        std::optional<SqliteError> pending_error_;

        friend class Command;
        friend class Query;
    };

    class Command {
    public:
        ~Command() = default;

        Command(const Command &) = delete;
        auto operator=(const Command &) -> Command & = delete;
        Command(Command &&) noexcept = default;
        auto operator=(Command &&) noexcept -> Command & = default;

        template <typename... Args>
        auto bind(Args &&...args) -> Command & {
            if (pending_error_.has_value()) {
                return *this;
            }
            if (consumed_) {
                pending_error_ = make_error(sqlite_error_kind::state_error, "statement is already consumed");
                return *this;
            }
            if (bound_) {
                pending_error_ = make_error(sqlite_error_kind::state_error, "statement is already bound");
                return *this;
            }
            statement_.bind_all(std::forward<Args>(args)...);
            bound_ = true;
            return *this;
        }

        [[nodiscard]]
        auto run() -> SqliteResult<void> {
            if (pending_error_.has_value()) {
                auto err = std::move(*pending_error_);
                pending_error_.reset();
                return std::unexpected(std::move(err));
            }
            if (auto latched = statement_.take_pending_error(); latched.has_value()) {
                return std::unexpected(std::move(*latched));
            }
            if (consumed_) {
                return fail(sqlite_error_kind::state_error, "statement is already consumed");
            }
            if (!bound_ && statement_.expected_bind_count() > 0) {
                if (auto check = statement_.try_validate_bind_count(0U); !check) {
                    return std::unexpected(check.error());
                }
            }
            if (statement_.result_columns() > 0) {
                return fail(sqlite_error_kind::state_error, "statement has result columns and must be executed as a query");
            }
            consumed_ = true;
            while (true) {
                auto stepped = statement_.step();
                if (!stepped) {
                    return std::unexpected(stepped.error());
                }
                if (!*stepped) {
                    break;
                }
            }
            return {};
        }

    private:
        explicit Command(Statement stmt) noexcept
        : statement_(std::move(stmt)) {}

        Statement statement_;
        bool bound_ = false;
        bool consumed_ = false;
        std::optional<SqliteError> pending_error_;

        friend class Database;
    };

    class Query {
    public:
        ~Query() = default;

        Query(const Query &) = delete;
        auto operator=(const Query &) -> Query & = delete;
        Query(Query &&) noexcept = default;
        auto operator=(Query &&) noexcept -> Query & = default;

        template <typename... Args>
        auto bind(Args &&...args) -> Query & {
            if (pending_error_.has_value()) {
                return *this;
            }
            if (consumed_) {
                pending_error_ = make_error(sqlite_error_kind::state_error, "statement is already consumed");
                return *this;
            }
            if (bound_) {
                pending_error_ = make_error(sqlite_error_kind::state_error, "statement is already bound");
                return *this;
            }
            statement_.bind_all(std::forward<Args>(args)...);
            bound_ = true;
            return *this;
        }

        template <typename T>
        [[nodiscard]]
        auto one() -> SqliteResult<T> {
            if (auto ready = ensure_ready(); !ready) {
                return std::unexpected(ready.error());
            }
            consumed_ = true;

            auto first_step = statement_.step();
            if (!first_step) {
                return std::unexpected(first_step.error());
            }
            if (!*first_step) {
                return fail(sqlite_error_kind::row_count_mismatch, "expected one row, got none");
            }

            auto row_or = statement_.row();
            if (!row_or) {
                return std::unexpected(row_or.error());
            }
            auto mapped = detail::map_row<T>(*row_or);

            auto second_step = statement_.step();
            if (!second_step) {
                return std::unexpected(second_step.error());
            }
            if (*second_step) {
                return fail(sqlite_error_kind::row_count_mismatch, "expected one row, got multiple");
            }

            return mapped;
        }

        template <typename T>
        [[nodiscard]]
        auto optional() -> SqliteResult<std::optional<T>> {
            if (auto ready = ensure_ready(); !ready) {
                return std::unexpected(ready.error());
            }
            consumed_ = true;

            auto first_step = statement_.step();
            if (!first_step) {
                return std::unexpected(first_step.error());
            }
            if (!*first_step) {
                return std::optional<T>{};
            }

            auto row_or = statement_.row();
            if (!row_or) {
                return std::unexpected(row_or.error());
            }
            auto mapped = detail::map_row<T>(*row_or);

            auto second_step = statement_.step();
            if (!second_step) {
                return std::unexpected(second_step.error());
            }
            if (*second_step) {
                return fail(sqlite_error_kind::row_count_mismatch, "expected one row, got multiple");
            }

            if (!mapped) {
                return std::unexpected(mapped.error());
            }
            return std::optional<T>{std::move(*mapped)};
        }

        template <typename T>
        [[nodiscard]]
        auto all() -> SqliteResult<std::vector<T>> {
            if (auto ready = ensure_ready(); !ready) {
                return std::unexpected(ready.error());
            }
            consumed_ = true;

            auto rows = std::vector<T>{};
            while (true) {
                auto stepped = statement_.step();
                if (!stepped) {
                    return std::unexpected(stepped.error());
                }
                if (!*stepped) {
                    break;
                }
                auto row_or = statement_.row();
                if (!row_or) {
                    return std::unexpected(row_or.error());
                }
                auto mapped = detail::map_row<T>(*row_or);
                if (!mapped) {
                    return std::unexpected(mapped.error());
                }
                rows.push_back(std::move(*mapped));
            }
            return rows;
        }

        template <typename Fn>
        [[nodiscard]]
        auto for_each(Fn fn) -> SqliteResult<void> {
            if (auto ready = ensure_ready(); !ready) {
                return ready;
            }
            consumed_ = true;
            while (true) {
                auto stepped = statement_.step();
                if (!stepped) {
                    return std::unexpected(stepped.error());
                }
                if (!*stepped) {
                    break;
                }
                auto row_or = statement_.row();
                if (!row_or) {
                    return std::unexpected(row_or.error());
                }
                std::invoke(fn, *row_or);
            }
            return {};
        }

    private:
        explicit Query(Statement stmt) noexcept
        : statement_(std::move(stmt)) {}

        [[nodiscard]]
        auto ensure_ready() -> SqliteResult<void> {
            if (pending_error_.has_value()) {
                auto err = std::move(*pending_error_);
                pending_error_.reset();
                return std::unexpected(std::move(err));
            }
            if (auto latched = statement_.take_pending_error(); latched.has_value()) {
                return std::unexpected(std::move(*latched));
            }
            if (consumed_) {
                return fail(sqlite_error_kind::state_error, "statement is already consumed");
            }
            if (!bound_ && statement_.expected_bind_count() > 0) {
                if (auto check = statement_.try_validate_bind_count(0U); !check) {
                    return check;
                }
            }
            if (statement_.result_columns() == 0) {
                return fail(sqlite_error_kind::state_error, "statement has zero result columns and must be executed as a command");
            }
            return {};
        }

        Statement statement_;
        bool bound_ = false;
        bool consumed_ = false;
        std::optional<SqliteError> pending_error_;

        friend class Database;
    };

    class Database {
    public:
        [[nodiscard]] static auto create(const std::filesystem::path &path) -> SqliteResult<Database>;

        ~Database() {
            close();
        }

        Database(const Database &) = delete;
        auto operator=(const Database &) -> Database & = delete;

        Database(Database &&other) noexcept
        : db_(std::exchange(other.db_, nullptr)),
          transaction_active_(std::exchange(other.transaction_active_, false)) {}

        auto operator=(Database &&other) noexcept -> Database & {
            if (this != &other) {
                close();
                db_ = std::exchange(other.db_, nullptr);
                transaction_active_ = std::exchange(other.transaction_active_, false);
            }
            return *this;
        }

        [[nodiscard]]
        auto exec(std::string_view sql) const -> SqliteResult<Command> {
            auto stmt_or = Statement::create(*this, sql);
            if (!stmt_or) {
                return std::unexpected(stmt_or.error());
            }
            return Command(std::move(*stmt_or));
        }

        [[nodiscard]]
        auto query(std::string_view sql) const -> SqliteResult<Query> {
            auto stmt_or = Statement::create(*this, sql);
            if (!stmt_or) {
                return std::unexpected(stmt_or.error());
            }
            return Query(std::move(*stmt_or));
        }

        [[nodiscard]]
        auto exec_script(std::string_view sql, std::string_view context = {}) const -> SqliteResult<void> {
            char *err_msg = nullptr;
            const auto sql_text = std::string(sql);
            const auto rc = sqlite3_exec(db_, sql_text.c_str(), nullptr, nullptr, &err_msg);
            if (rc == SQLITE_OK) {
                sqlite3_free(err_msg);
                return {};
            }
            std::string diagnostic;
            if (err_msg != nullptr) {
                diagnostic = err_msg;
            } else if (db_ != nullptr) {
                const auto *fallback = sqlite3_errmsg(db_);
                if (fallback != nullptr) {
                    diagnostic = fallback;
                } else {
                    diagnostic = "unknown error";
                }
            } else {
                diagnostic = "unknown error";
            }
            sqlite3_free(err_msg);
            const auto prefix = context.empty() ? std::string_view{"sqlite exec script failed"} : context;
            return fail(sqlite_error_kind::step_failed, format_sqlite_message(prefix, diagnostic), rc);
        }

        [[nodiscard]]
        auto try_exec_script(std::string_view sql) const noexcept -> bool {
            char *err_msg = nullptr;
            const auto sql_text = std::string(sql);
            const auto rc = sqlite3_exec(db_, sql_text.c_str(), nullptr, nullptr, &err_msg);
            sqlite3_free(err_msg);
            return rc == SQLITE_OK;
        }

        [[nodiscard]]
        auto table_exists(std::string_view table_name) const -> SqliteResult<bool> {
            auto stmt_or = Statement::create(*this, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1");
            if (!stmt_or) {
                return std::unexpected(stmt_or.error());
            }
            auto &stmt = *stmt_or;
            if (auto bind_r = stmt.try_bind(1, table_name); !bind_r) {
                return std::unexpected(bind_r.error());
            }
            auto step_r = stmt.step();
            if (!step_r) {
                return std::unexpected(step_r.error());
            }
            return *step_r;
        }

        template <typename Fn>
        [[nodiscard]]
        auto transaction(Fn &&fn);

        [[nodiscard]]
        auto changes() const noexcept -> int {
            return db_ != nullptr ? sqlite3_changes(db_) : 0;
        }

        [[nodiscard]]
        auto handle() const noexcept -> sqlite3 * {
            return db_;
        }

    private:
        explicit Database(sqlite3 *db) noexcept
        : db_(db) {}

        void close() noexcept {
            if (db_ != nullptr) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            transaction_active_ = false;
        }

        sqlite3 *db_ = nullptr;
        mutable bool transaction_active_ = false;

        friend class Transaction;
    };

    class Transaction {
    public:
        [[nodiscard]] static auto create(Database &db) -> SqliteResult<Transaction>;

        ~Transaction() {
            rollback_if_pending();
        }

        Transaction(const Transaction &) = delete;
        auto operator=(const Transaction &) -> Transaction & = delete;

        Transaction(Transaction &&other) noexcept
        : db_(std::exchange(other.db_, nullptr)),
          committed_(std::exchange(other.committed_, true)) {}

        auto operator=(Transaction &&other) noexcept -> Transaction & {
            if (this != &other) {
                rollback_if_pending();
                db_ = std::exchange(other.db_, nullptr);
                committed_ = std::exchange(other.committed_, true);
            }
            return *this;
        }

        [[nodiscard]]
        auto commit() -> SqliteResult<void> {
            if (committed_) {
                return fail(sqlite_error_kind::state_error, "transaction already committed");
            }
            if (db_ == nullptr) {
                return fail(sqlite_error_kind::state_error, "transaction is empty");
            }
            auto result = db_->exec_script("COMMIT;", "sqlite transaction commit failed");
            if (!result) {
                return std::unexpected(result.error());
            }
            committed_ = true;
            db_->transaction_active_ = false;
            return {};
        }

    private:
        explicit Transaction(Database *db) noexcept
        : db_(db) {}

        void rollback_if_pending() noexcept {
            if (db_ == nullptr || committed_) {
                return;
            }
            static_cast<void>(db_->try_exec_script("ROLLBACK;"));
            db_->transaction_active_ = false;
            db_ = nullptr;
        }

        Database *db_ = nullptr;
        bool committed_ = false;

        friend class Database;
    };

    namespace detail {

        template <typename T>
        concept has_custom_row_mapper = requires(const Row &row) {
            { RowMapper<T>::map(row) } -> std::same_as<SqliteResult<T>>;
        };

        template <typename T, std::size_t... Indices>
        [[nodiscard]]
        auto map_tuple(const Row &row, std::index_sequence<Indices...>) -> SqliteResult<T> {
            constexpr auto EXPECTED_COLUMNS = sizeof...(Indices);
            if (row.columns() != static_cast<int>(EXPECTED_COLUMNS)) {
                return fail(sqlite_error_kind::mapping_error, column_count_message(EXPECTED_COLUMNS, row.columns()));
            }
            std::optional<SqliteError> captured;
            auto fetch = [&]<std::size_t I, typename Elem>() -> Elem {
                if (captured.has_value()) {
                    return Elem{};
                }
                auto value_or = row.template get<Elem>(static_cast<int>(I));
                if (!value_or) {
                    captured = value_or.error();
                    return Elem{};
                }
                return std::move(*value_or);
            };
            T result{fetch.template operator()<Indices, std::tuple_element_t<Indices, T>>()...};
            if (captured.has_value()) {
                return std::unexpected(std::move(*captured));
            }
            return result;
        }

        template <typename T>
        [[nodiscard]]
        auto map_row(const Row &row) -> SqliteResult<T> {
            if constexpr (has_custom_row_mapper<T>) {
                return RowMapper<T>::map(row);
            } else if constexpr (tuple_like<T>) {
                return map_tuple<T>(row, std::make_index_sequence<std::tuple_size_v<T>>{});
            } else {
                if (row.columns() != 1) {
                    return fail(sqlite_error_kind::mapping_error, column_count_message(1U, row.columns()));
                }
                return row.template get<T>(0);
            }
        }

    } // namespace detail

    /// Read the first `sizeof...(Ts)` columns of `row` as the given types, short-circuiting
    /// on the first failure. The row's column count must match exactly. Intended for
    /// `RowMapper::map` bodies that would otherwise repeat `row.get<T>(i)` + early-return
    /// boilerplate for each field.
    template <typename... Ts>
    [[nodiscard]]
    auto read_columns(const Row &row) -> SqliteResult<std::tuple<Ts...>> {
        return detail::map_tuple<std::tuple<Ts...>>(row, std::index_sequence_for<Ts...>{});
    }

    inline auto Statement::create(const Database &db, std::string_view sql) -> SqliteResult<Statement> {
        auto stmt_or = detail::prepare_statement(db.handle(), sql);
        if (!stmt_or) {
            return std::unexpected(stmt_or.error());
        }
        return Statement{*stmt_or, detail::analyze_placeholders(sql)};
    }

    inline auto Transaction::create(Database &db) -> SqliteResult<Transaction> {
        if (db.transaction_active_) {
            return fail(sqlite_error_kind::state_error, "transaction already active");
        }
        db.transaction_active_ = true;
        auto begin = db.exec_script("BEGIN IMMEDIATE TRANSACTION;", "sqlite transaction begin failed");
        if (!begin) {
            db.transaction_active_ = false;
            return std::unexpected(begin.error());
        }
        return Transaction{&db};
    }

    inline auto Database::create(const std::filesystem::path &path) -> SqliteResult<Database> {
        try {
            const auto parent = path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
        } catch (const std::filesystem::filesystem_error &e) {
            return fail(sqlite_error_kind::open_failed,
                        std::string("failed to create parent directory: ") + e.what());
        }

        sqlite3 *handle = nullptr;
        const auto path_text = path.string();
        const auto open_rc = sqlite3_open(path_text.c_str(), &handle);
        if (open_rc != SQLITE_OK) {
            std::string message = "failed to open sqlite database: " + path_text;
            if (handle != nullptr) {
                message += ": ";
                const auto *errmsg = sqlite3_errmsg(handle);
                message += errmsg != nullptr ? errmsg : "unknown error";
                sqlite3_close(handle);
            }
            return fail(sqlite_error_kind::open_failed, std::move(message), open_rc);
        }

        const auto timeout_rc = sqlite3_busy_timeout(handle, detail::DEFAULT_BUSY_TIMEOUT_MS);
        if (timeout_rc != SQLITE_OK) {
            std::string message = "failed to configure sqlite busy timeout";
            const auto *errmsg = sqlite3_errmsg(handle);
            if (errmsg != nullptr) {
                message += ": ";
                message += errmsg;
            }
            sqlite3_close(handle);
            return fail(sqlite_error_kind::open_failed, std::move(message), timeout_rc);
        }

        return Database{handle};
    }

    namespace detail {

        template <typename T>
        concept is_sqlite_result = requires {
            typename T::value_type;
            typename T::error_type;
            requires std::same_as<typename T::error_type, SqliteError>;
            requires std::same_as<T, SqliteResult<typename T::value_type>>;
        };

    } // namespace detail

    template <typename Fn>
    [[nodiscard]]
    inline auto Database::transaction(Fn &&fn) {
        using raw_return = std::invoke_result_t<Fn, Database &>;

        auto tx_or = Transaction::create(*this);

        if constexpr (std::is_void_v<raw_return>) {
            using final_result = SqliteResult<void>;
            if (!tx_or) {
                return final_result{std::unexpected(tx_or.error())};
            }
            std::invoke(std::forward<Fn>(fn), *this);
            return tx_or->commit();
        } else if constexpr (detail::is_sqlite_result<raw_return>) {
            if (!tx_or) {
                return raw_return{std::unexpected(tx_or.error())};
            }
            auto body_result = std::invoke(std::forward<Fn>(fn), *this);
            if (!body_result) {
                return body_result;
            }
            auto commit_result = tx_or->commit();
            if (!commit_result) {
                return raw_return{std::unexpected(commit_result.error())};
            }
            return body_result;
        } else {
            using final_result = SqliteResult<raw_return>;
            if (!tx_or) {
                return final_result{std::unexpected(tx_or.error())};
            }
            auto body_value = std::invoke(std::forward<Fn>(fn), *this);
            auto commit_result = tx_or->commit();
            if (!commit_result) {
                return final_result{std::unexpected(commit_result.error())};
            }
            return final_result{std::move(body_value)};
        }
    }

} // namespace orangutan::sqlite
