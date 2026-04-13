#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "types/base.hpp"

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

        enum class placeholder_mode : base::u8 {
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

        inline auto sqlite_error(sqlite3 *db, std::string_view fallback = "unknown error") -> std::string {
            const auto *message = sqlite3_errmsg(db);
            return message != nullptr ? std::string(message) : std::string(fallback);
        }

        inline void throw_sqlite_error(std::string_view prefix, sqlite3 *db) {
            throw std::runtime_error(std::string(prefix) + ": " + sqlite_error(db));
        }

        inline void check_sqlite_ok(int rc, sqlite3 *db, std::string_view context) {
            if (rc != SQLITE_OK) {
                throw_sqlite_error(context, db);
            }
        }

        inline void check_bind_ok(int rc, sqlite3_stmt *stmt) {
            if (rc != SQLITE_OK) {
                throw std::runtime_error("sqlite bind failed: " + sqlite_error(sqlite3_db_handle(stmt)));
            }
        }

        inline auto is_space(char ch) -> bool {
            return std::isspace(static_cast<unsigned char>(ch)) != 0;
        }

        inline auto prepare_statement(sqlite3 *db, std::string_view sql) -> sqlite3_stmt * {
            auto sql_text = std::string(sql);
            sqlite3_stmt *stmt = nullptr;
            const char *tail = nullptr;
            const auto rc = sqlite3_prepare_v2(db, sql_text.c_str(), static_cast<int>(sql_text.size()), &stmt, &tail);
            if (rc != SQLITE_OK) {
                throw std::runtime_error("sqlite prepare failed: " + sqlite_error(db));
            }
            if (stmt == nullptr) {
                throw std::runtime_error("sqlite prepare failed: empty SQL");
            }
            while (tail != nullptr && *tail != '\0' && is_space(*tail)) {
                ++tail;
            }
            if (tail != nullptr && *tail != '\0') {
                sqlite3_finalize(stmt);
                throw std::runtime_error("sqlite prepare failed: trailing SQL");
            }
            return stmt;
        }

        inline auto analyze_placeholders(std::string_view sql) -> PlaceholderInfo {
            auto info = PlaceholderInfo{};
            std::vector<int> indexed_ids;
            int anonymous_count = 0;

            enum class parse_state : base::u8 {
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
                            auto tail = index + 1;
                            while (tail < sql.size() && std::isdigit(static_cast<unsigned char>(sql[tail])) != 0) {
                                ++tail;
                            }

                            if (tail == index + 1) {
                                ++anonymous_count;
                            } else {
                                indexed_ids.push_back(std::stoi(std::string(sql.substr(index + 1, tail - index - 1))));
                            }
                            index = tail - 1;
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

        inline auto bind_count_message(std::size_t expected, std::size_t actual) -> std::string {
            return "bind parameter count mismatch: expected " + std::to_string(expected) + ", got " + std::to_string(actual);
        }

        inline auto column_count_message(std::size_t expected, int actual) -> std::string {
            if (expected == 1U) {
                return "expected one column, got " + std::to_string(actual);
            }
            return "expected " + std::to_string(expected) + " columns, got " + std::to_string(actual);
        }

        inline void validate_column_index(sqlite3_stmt *stmt, int index) {
            if (index < 0 || index >= sqlite3_column_count(stmt)) {
                throw std::runtime_error("column index out of range");
            }
        }

        inline auto copy_column_text(sqlite3_stmt *stmt, int index) -> std::string {
            const auto *text = sqlite3_column_text(stmt, index);
            if (text == nullptr) {
                return {};
            }
            const auto size = static_cast<std::size_t>(sqlite3_column_bytes(stmt, index));
            return {reinterpret_cast<const char *>(text), size}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        }

        template <typename T>
        void bind_value(sqlite3_stmt *stmt, int index, T &&value) {
            using value_type = std::remove_cvref_t<T>;

            if constexpr (requires {
                              typename value_type::value_type;
                              requires std::same_as<value_type, std::optional<typename value_type::value_type>>;
                          }) {
                if (value.has_value()) {
                    bind_value(stmt, index, *std::forward<T>(value));
                } else {
                    check_bind_ok(sqlite3_bind_null(stmt, index), stmt);
                }
            } else if constexpr (std::same_as<value_type, std::nullptr_t>) {
                check_bind_ok(sqlite3_bind_null(stmt, index), stmt);
            } else if constexpr (std::same_as<value_type, bool>) {
                check_bind_ok(sqlite3_bind_int(stmt, index, value ? 1 : 0), stmt);
            } else if constexpr (std::same_as<value_type, int>) {
                check_bind_ok(sqlite3_bind_int(stmt, index, value), stmt);
            } else if constexpr (std::same_as<value_type, std::int64_t>) {
                check_bind_ok(sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(value)), stmt);
            } else if constexpr (std::same_as<value_type, double>) {
                check_bind_ok(sqlite3_bind_double(stmt, index, value), stmt);
            } else if constexpr (std::is_pointer_v<value_type> && std::same_as<std::remove_cv_t<std::remove_pointer_t<value_type>>, char>) {
                if (value == nullptr) {
                    check_bind_ok(sqlite3_bind_null(stmt, index), stmt);
                } else {
                    check_bind_ok(sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT), stmt);
                }
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
                check_bind_ok(sqlite3_bind_text(stmt, index, text, static_cast<int>(text_value.size()), SQLITE_TRANSIENT), stmt);
            } else {
                static_assert(false, "unsupported sqlite bind type");
            }
        }

        template <typename T>
        auto column_value(sqlite3_stmt *stmt, int index) -> T {
            validate_column_index(stmt, index);
            using value_type = std::remove_cvref_t<T>;

            if constexpr (requires {
                              typename value_type::value_type;
                              requires std::same_as<value_type, std::optional<typename value_type::value_type>>;
                          }) {
                if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
                    return std::nullopt;
                }
                return std::optional<typename value_type::value_type>{column_value<typename value_type::value_type>(stmt, index)};
            } else {
                if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
                    throw std::runtime_error("column " + std::to_string(index) + " is null");
                }
                if constexpr (std::same_as<value_type, std::string>) {
                    return copy_column_text(stmt, index);
                } else if constexpr (std::same_as<value_type, int>) {
                    return sqlite3_column_int(stmt, index);
                } else if constexpr (std::same_as<value_type, std::int64_t> || std::same_as<value_type, base::i64>) {
                    return static_cast<value_type>(sqlite3_column_int64(stmt, index));
                } else if constexpr (std::same_as<value_type, double> || std::same_as<value_type, base::f64>) {
                    return static_cast<value_type>(sqlite3_column_double(stmt, index));
                } else if constexpr (std::same_as<value_type, bool>) {
                    return sqlite3_column_int(stmt, index) != 0;
                } else {
                    static_assert(false, "unsupported sqlite column type");
                }
            }
        }

        template <typename T>
        auto map_row(const Row &row) -> T;

    } // namespace detail

    class Row {
    public:
        template <typename T>
        [[nodiscard]]
        auto get(int index) const -> T {
            return detail::column_value<T>(stmt_, index);
        }

        [[nodiscard]]
        auto is_null(int index) const -> bool {
            detail::validate_column_index(stmt_, index);
            return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
        }

        [[nodiscard]]
        auto columns() const -> int {
            return sqlite3_column_count(stmt_);
        }

    private:
        explicit Row(sqlite3_stmt *stmt)
        : stmt_(stmt) {}

        sqlite3_stmt *stmt_ = nullptr;

        friend class Statement;
    };

    class Statement {
    public:
        Statement(const Database &db, std::string_view sql);
        ~Statement();

        Statement(const Statement &) = delete;
        auto operator=(const Statement &) -> Statement & = delete;
        Statement(Statement &&other) noexcept;
        auto operator=(Statement &&other) noexcept -> Statement &;

        template <typename T>
        auto bind(int index, T &&value) -> Statement & {
            detail::bind_value(stmt_, index, std::forward<T>(value));
            return *this;
        }

        template <typename... Args>
        auto bind_all(Args &&...args) -> Statement & {
            validate_bind_count(sizeof...(Args));
            int parameter_index = 1;
            (bind(parameter_index++, std::forward<Args>(args)), ...);
            return *this;
        }

        [[nodiscard]]
        auto step() -> bool;

        [[nodiscard]]
        auto row() const -> Row;

        void reset();
        void clear_bindings();

    private:
        void validate_bind_count(std::size_t actual) const;

        [[nodiscard]]
        auto result_columns() const -> int {
            return sqlite3_column_count(stmt_);
        }

        [[nodiscard]]
        auto expected_bind_count() const -> int {
            return placeholder_info_.expected_bind_count;
        }

        sqlite3_stmt *stmt_ = nullptr;
        detail::PlaceholderInfo placeholder_info_{};
        bool has_row_ = false;

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
            ensure_can_bind();
            statement_.bind_all(std::forward<Args>(args)...);
            bound_ = true;
            return *this;
        }

        void run();

    private:
        explicit Command(const Database &db, std::string_view sql)
        : statement_(db, sql) {}

        void ensure_can_bind() const;
        void ensure_ready();

        Statement statement_;
        bool bound_ = false;
        bool consumed_ = false;

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
            ensure_can_bind();
            statement_.bind_all(std::forward<Args>(args)...);
            bound_ = true;
            return *this;
        }

        template <typename T>
        auto one() -> T {
            ensure_ready();
            consumed_ = true;
            if (!statement_.step()) {
                throw std::runtime_error("expected one row, got none");
            }
            auto mapping_error = std::exception_ptr{};
            auto row_value = std::optional<T>{};
            try {
                row_value.emplace(detail::map_row<T>(statement_.row()));
            } catch (...) {
                mapping_error = std::current_exception();
            }
            if (statement_.step()) {
                throw std::runtime_error("expected one row, got multiple");
            }
            if (mapping_error != nullptr) {
                std::rethrow_exception(mapping_error);
            }
            return std::move(*row_value);
        }

        template <typename T>
        auto optional() -> std::optional<T> {
            ensure_ready();
            consumed_ = true;
            if (!statement_.step()) {
                return std::nullopt;
            }
            auto mapping_error = std::exception_ptr{};
            auto row_value = std::optional<T>{};
            try {
                row_value.emplace(detail::map_row<T>(statement_.row()));
            } catch (...) {
                mapping_error = std::current_exception();
            }
            if (statement_.step()) {
                throw std::runtime_error("expected one row, got multiple");
            }
            if (mapping_error != nullptr) {
                std::rethrow_exception(mapping_error);
            }
            return row_value;
        }

        template <typename T>
        auto all() -> std::vector<T> {
            ensure_ready();
            consumed_ = true;
            auto rows = std::vector<T>{};
            while (statement_.step()) {
                rows.push_back(detail::map_row<T>(statement_.row()));
            }
            return rows;
        }

        template <typename Fn>
        void for_each(Fn &&fn) {
            ensure_ready();
            consumed_ = true;
            auto callback = [&]() {
                if constexpr (std::is_lvalue_reference_v<Fn &&>) {
                    return std::ref(std::forward<Fn>(fn));
                } else {
                    return std::forward<Fn>(fn);
                }
            }();
            while (statement_.step()) {
                std::invoke(callback, statement_.row());
            }
        }

    private:
        explicit Query(const Database &db, std::string_view sql)
        : statement_(db, sql) {}

        void ensure_can_bind() const;
        void ensure_ready();

        Statement statement_;
        bool bound_ = false;
        bool consumed_ = false;

        friend class Database;
    };

    class Database {
    public:
        explicit Database(const std::filesystem::path &path);
        ~Database();

        Database(const Database &) = delete;
        auto operator=(const Database &) -> Database & = delete;
        Database(Database &&) = delete;
        auto operator=(Database &&) -> Database & = delete;

        [[nodiscard]]
        auto exec(std::string_view sql) const -> Command;

        [[nodiscard]]
        auto query(std::string_view sql) const -> Query;

        void exec_script(std::string_view sql, std::string_view context) const;
        [[nodiscard]]
        auto try_exec_script(std::string_view sql) const -> bool;
        [[nodiscard]]
        auto table_exists(std::string_view table_name) const -> bool;

        template <typename Fn>
        decltype(auto) transaction(Fn &&fn);

        [[nodiscard]]
        auto changes() const -> int;

        [[nodiscard]]
        auto handle() const noexcept -> sqlite3 *;

    private:
        void begin_transaction_scope();
        void end_transaction_scope() noexcept;

        sqlite3 *db_ = nullptr;
        bool transaction_active_ = false;

        friend class Transaction;
    };

    class Transaction {
    public:
        explicit Transaction(Database &db);
        ~Transaction();

        Transaction(const Transaction &) = delete;
        auto operator=(const Transaction &) -> Transaction & = delete;
        Transaction(Transaction &&) = delete;
        auto operator=(Transaction &&) -> Transaction & = delete;

        void commit();

    private:
        Database *db_ = nullptr;
        bool committed_ = false;
    };

    namespace detail {

        template <typename T>
        concept has_custom_row_mapper = requires(const Row &row) {
            RowMapper<T>::map(row);
        };

        template <typename T, std::size_t... Indices>
        auto map_tuple(const Row &row, std::index_sequence<Indices...>) -> T {
            constexpr auto EXPECTED_COLUMNS = sizeof...(Indices);
            if (row.columns() != static_cast<int>(EXPECTED_COLUMNS)) {
                throw std::runtime_error(column_count_message(EXPECTED_COLUMNS, row.columns()));
            }
            return T{row.template get<std::tuple_element_t<Indices, T>>(static_cast<int>(Indices))...};
        }

        template <typename T>
        auto map_row(const Row &row) -> T {
            if constexpr (has_custom_row_mapper<T>) {
                return RowMapper<T>::map(row);
            } else if constexpr (tuple_like<T>) {
                return map_tuple<T>(row, std::make_index_sequence<std::tuple_size_v<T>>{});
            } else {
                if (row.columns() != 1) {
                    throw std::runtime_error(column_count_message(1U, row.columns()));
                }
                return row.template get<T>(0);
            }
        }

    } // namespace detail

    inline Statement::Statement(const Database &db, std::string_view sql)
    : stmt_(detail::prepare_statement(db.handle(), sql)),
      placeholder_info_(detail::analyze_placeholders(sql)) {}

    inline Statement::~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    inline Statement::Statement(Statement &&other) noexcept
    : stmt_(std::exchange(other.stmt_, nullptr)),
      placeholder_info_(other.placeholder_info_),
      has_row_(other.has_row_) {
        other.has_row_ = false;
    }

    inline auto Statement::operator=(Statement &&other) noexcept -> Statement & {
        if (this != &other) {
            if (stmt_ != nullptr) {
                sqlite3_finalize(stmt_);
            }
            stmt_ = std::exchange(other.stmt_, nullptr);
            placeholder_info_ = other.placeholder_info_;
            has_row_ = other.has_row_;
            other.has_row_ = false;
        }
        return *this;
    }

    inline auto Statement::step() -> bool {
        const auto rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) {
            has_row_ = true;
            return true;
        }
        if (rc == SQLITE_DONE) {
            has_row_ = false;
            return false;
        }
        detail::throw_sqlite_error("sqlite step failed", sqlite3_db_handle(stmt_));
        return false;
    }

    inline auto Statement::row() const -> Row {
        if (!has_row_) {
            throw std::runtime_error("statement does not point at a row");
        }
        return Row(stmt_);
    }

    inline void Statement::reset() {
        static_cast<void>(sqlite3_reset(stmt_));
        has_row_ = false;
    }

    inline void Statement::clear_bindings() {
        detail::check_sqlite_ok(sqlite3_clear_bindings(stmt_), sqlite3_db_handle(stmt_), "sqlite clear bindings failed");
    }

    inline void Statement::validate_bind_count(std::size_t actual) const {
        if (placeholder_info_.mode == detail::placeholder_mode::mixed) {
            throw std::runtime_error("mixed placeholder styles are not supported");
        }
        if (placeholder_info_.mode == detail::placeholder_mode::indexed && !placeholder_info_.indexed_contiguous) {
            throw std::runtime_error("indexed placeholders must be contiguous");
        }
        if (std::cmp_not_equal(actual, placeholder_info_.expected_bind_count)) {
            throw std::runtime_error(detail::bind_count_message(static_cast<std::size_t>(placeholder_info_.expected_bind_count), actual));
        }
    }

    inline void Command::ensure_can_bind() const {
        if (consumed_) {
            throw std::runtime_error("statement is already consumed");
        }
        if (bound_) {
            throw std::runtime_error("statement is already bound");
        }
    }

    inline void Command::ensure_ready() {
        if (consumed_) {
            throw std::runtime_error("statement is already consumed");
        }
        if (!bound_ && statement_.expected_bind_count() > 0) {
            statement_.validate_bind_count(0U);
        }
        if (statement_.result_columns() > 0) {
            throw std::runtime_error("statement has result columns and must be executed as a query");
        }
    }

    inline void Command::run() {
        ensure_ready();
        consumed_ = true;
        while (statement_.step()) {
        }
    }

    inline void Query::ensure_can_bind() const {
        if (consumed_) {
            throw std::runtime_error("statement is already consumed");
        }
        if (bound_) {
            throw std::runtime_error("statement is already bound");
        }
    }

    inline void Query::ensure_ready() {
        if (consumed_) {
            throw std::runtime_error("statement is already consumed");
        }
        if (!bound_ && statement_.expected_bind_count() > 0) {
            statement_.validate_bind_count(0U);
        }
        if (statement_.result_columns() == 0) {
            throw std::runtime_error("statement has zero result columns and must be executed as a command");
        }
    }

    inline Database::Database(const std::filesystem::path &path) {
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        const auto path_text = path.string();
        if (sqlite3_open(path_text.c_str(), &db_) != SQLITE_OK) {
            const auto message = detail::sqlite_error(db_);
            if (db_ != nullptr) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error("failed to open sqlite database: " + path_text + ": " + message);
        }

        try {
            detail::check_sqlite_ok(sqlite3_busy_timeout(db_, detail::DEFAULT_BUSY_TIMEOUT_MS), db_, "failed to configure sqlite busy timeout");
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }

    inline Database::~Database() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    inline auto Database::exec(std::string_view sql) const -> Command {
        return Command(*this, sql);
    }

    inline auto Database::query(std::string_view sql) const -> Query {
        return Query(*this, sql);
    }

    inline void Database::exec_script(std::string_view sql, std::string_view context) const {
        char *err_msg = nullptr;
        const auto rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            const auto message = err_msg != nullptr ? std::string(err_msg) : detail::sqlite_error(db_);
            sqlite3_free(err_msg);
            if (context.empty()) {
                throw std::runtime_error("sqlite exec script failed: " + message);
            }
            throw std::runtime_error(std::string(context) + ": " + message);
        }
    }

    inline auto Database::try_exec_script(std::string_view sql) const -> bool {
        char *err_msg = nullptr;
        const auto rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &err_msg);
        sqlite3_free(err_msg);
        return rc == SQLITE_OK;
    }

    inline auto Database::table_exists(std::string_view table_name) const -> bool {
        Statement stmt(*this, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1");
        stmt.bind(1, table_name);
        return stmt.step();
    }

    template <typename Fn>
    decltype(auto) Database::transaction(Fn &&fn) {
        Transaction tx(*this);
        if constexpr (std::is_void_v<std::invoke_result_t<Fn, Database &>>) {
            std::invoke(std::forward<Fn>(fn), *this);
            tx.commit();
            return;
        } else {
            auto result = std::invoke(std::forward<Fn>(fn), *this);
            tx.commit();
            return result;
        }
    }

    inline auto Database::changes() const -> int {
        return sqlite3_changes(db_);
    }

    inline auto Database::handle() const noexcept -> sqlite3 * {
        return db_;
    }

    inline void Database::begin_transaction_scope() {
        if (transaction_active_) {
            throw std::runtime_error("transaction already active");
        }
        transaction_active_ = true;
    }

    inline void Database::end_transaction_scope() noexcept {
        transaction_active_ = false;
    }

    inline Transaction::Transaction(Database &db)
    : db_(&db) {
        db_->begin_transaction_scope();
        try {
            db_->exec_script("BEGIN IMMEDIATE TRANSACTION;", "sqlite transaction begin failed");
        } catch (...) {
            db_->end_transaction_scope();
            throw;
        }
    }

    inline Transaction::~Transaction() {
        if (db_ == nullptr) {
            return;
        }
        if (!committed_) {
            static_cast<void>(db_->try_exec_script("ROLLBACK;"));
        }
        db_->end_transaction_scope();
    }

    inline void Transaction::commit() {
        if (committed_) {
            throw std::runtime_error("transaction already committed");
        }
        db_->exec_script("COMMIT;", "sqlite transaction commit failed");
        committed_ = true;
    }

} // namespace orangutan::sqlite
