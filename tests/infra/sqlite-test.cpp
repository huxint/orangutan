#include "infra/storage/sqlite.hpp"

#include <filesystem>
#include <gtest/gtest.h>

using namespace orangutan;

class SqliteTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / "orangutan_sqlite_test.db";
        std::filesystem::remove(db_path_);
    }

    void TearDown() override {
        std::filesystem::remove(db_path_);
    }

    [[nodiscard]]
    const std::filesystem::path &db_path() const {
        return db_path_;
    }

private:
    std::filesystem::path db_path_;
};

TEST_F(SqliteTest, DatabaseExecAndStatementRoundTripValues) {
    sqlite::Database db(db_path().string());
    db.exec("CREATE TABLE sample (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL NOT NULL);", "create table failed");

    sqlite::Statement insert(db, "INSERT INTO sample (name, score) VALUES (?, ?)");
    insert.bind_text(1, "alice");
    insert.bind_double(2, 9.5);
    (void)insert.step();

    sqlite::Statement query(db, "SELECT name, score FROM sample LIMIT 1");
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.column_text(0), "alice");
    EXPECT_DOUBLE_EQ(query.column_double(1), 9.5);
}

TEST_F(SqliteTest, TransactionRollsBackWhenNotCommitted) {
    sqlite::Database db(db_path().string());
    db.exec("CREATE TABLE sample (value TEXT NOT NULL);", "create table failed");

    {
        sqlite::Transaction tx(db);
        sqlite::Statement insert(db, "INSERT INTO sample (value) VALUES (?)");
        insert.bind_text(1, "transient");
        (void)insert.step();
    }

    sqlite::Statement query(db, "SELECT COUNT(*) FROM sample");
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.column_int(0), 0);
}

TEST_F(SqliteTest, DatabaseConfiguresBusyTimeout) {
    sqlite::Database db(db_path().string());

    sqlite::Statement query(db, "PRAGMA busy_timeout");
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.column_int(0), 1000);
}
