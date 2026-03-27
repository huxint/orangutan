#pragma once

#include <filesystem>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace orangutan::sqlite {

    class Database {
    public:
        explicit Database(const std::filesystem::path &path);
        ~Database();

        Database(const Database &) = delete;
        Database &operator=(const Database &) = delete;
        Database(Database &&) = delete;
        Database &operator=(Database &&) = delete;

        void exec(std::string_view sql, std::string_view context) const;
        [[nodiscard]]
        bool try_exec(std::string_view sql) const;
        [[nodiscard]]
        bool table_exists(std::string_view table_name) const;
        [[nodiscard]]
        int changes() const;
        [[nodiscard]]
        sqlite3 *handle() const noexcept;

    private:
        sqlite3 *db_ = nullptr;
    };

    class Statement {
    public:
        Statement(const Database &db, std::string_view sql);
        ~Statement();

        Statement(const Statement &) = delete;
        Statement &operator=(const Statement &) = delete;
        Statement(Statement &&) = delete;
        Statement &operator=(Statement &&) = delete;

        void bind_text(int index, std::string_view value);
        void bind_int(int index, int value);
        void bind_double(int index, double value);
        [[nodiscard]]
        bool step();
        [[nodiscard]]
        std::string column_text(int index) const;
        [[nodiscard]]
        int column_int(int index) const;
        [[nodiscard]]
        double column_double(int index) const;
        void reset();

    private:
        sqlite3_stmt *stmt_ = nullptr;
    };

    class Transaction {
    public:
        explicit Transaction(const Database &db);
        ~Transaction();

        Transaction(const Transaction &) = delete;
        Transaction &operator=(const Transaction &) = delete;
        Transaction(Transaction &&) = delete;
        Transaction &operator=(Transaction &&) = delete;

        void commit();

    private:
        const Database &db_;
        bool committed_ = false;
    };

} // namespace orangutan::sqlite
