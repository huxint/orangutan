#include "infra/storage/sqlite.hpp"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <sqlite3.h>
#include <stdexcept>

namespace orangutan::sqlite {

namespace {

constexpr auto default_busy_timeout_ms = 1000;

std::string sqlite_error(sqlite3 *db, std::string_view fallback = "unknown error") {
    const auto *message = sqlite3_errmsg(db);
    return message != nullptr ? std::string(message) : std::string(fallback);
}

} // namespace

Database::Database(const std::string &path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("Failed to open sqlite database: " + path + ": " + sqlite_error(db_));
    }
    if (sqlite3_busy_timeout(db_, default_busy_timeout_ms) != SQLITE_OK) {
        throw std::runtime_error("Failed to configure sqlite busy timeout: " + sqlite_error(db_));
    }
}

Database::~Database() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void Database::exec(std::string_view sql, std::string_view context) const {
    char *err_msg = nullptr;
    if (sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
        const auto err = err_msg != nullptr ? std::string(err_msg) : sqlite_error(db_);
        sqlite3_free(err_msg);
        throw std::runtime_error(std::string(context) + ": " + err);
    }
}

bool Database::try_exec(std::string_view sql) const {
    char *err_msg = nullptr;
    const auto rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &err_msg);
    sqlite3_free(err_msg);
    return rc == SQLITE_OK;
}

bool Database::table_exists(std::string_view table_name) const {
    Statement stmt(*this, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1");
    stmt.bind_text(1, table_name);
    return stmt.step();
}

int Database::changes() const {
    return sqlite3_changes(db_);
}

sqlite3 *Database::handle() const noexcept {
    return db_;
}

Statement::Statement(const Database &db, std::string_view sql) {
    if (sqlite3_prepare_v2(db.handle(), std::string(sql).c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
        throw std::runtime_error("SQLite prepare failed: " + sqlite_error(db.handle()));
    }
}

Statement::~Statement() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
}

void Statement::bind_text(int index, std::string_view value) {
    sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void Statement::bind_int(int index, int value) {
    sqlite3_bind_int(stmt_, index, value);
}

void Statement::bind_double(int index, double value) {
    sqlite3_bind_double(stmt_, index, value);
}

bool Statement::step() {
    const auto rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw std::runtime_error("SQLite step failed: " + sqlite_error(sqlite3_db_handle(stmt_)));
}

std::string Statement::column_text(int index) const {
    const auto *text = sqlite3_column_text(stmt_, index);
    if (text == nullptr) {
        return {};
    }

    const auto size = static_cast<size_t>(sqlite3_column_bytes(stmt_, index));
    std::string result;
    result.reserve(size);
    std::ranges::copy_n(text, static_cast<std::ptrdiff_t>(size), std::back_inserter(result));
    return result;
}

int Statement::column_int(int index) const {
    return sqlite3_column_int(stmt_, index);
}

double Statement::column_double(int index) const {
    return sqlite3_column_double(stmt_, index);
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

Transaction::Transaction(const Database &db)
: db_(db) {
    db_.exec("BEGIN IMMEDIATE TRANSACTION;", "SQLite transaction begin failed");
}

Transaction::~Transaction() {
    if (!committed_) {
        static_cast<void>(db_.try_exec("ROLLBACK;"));
    }
}

void Transaction::commit() {
    db_.exec("COMMIT;", "SQLite transaction commit failed");
    committed_ = true;
}

} // namespace orangutan::sqlite
