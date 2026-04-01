#include "storage/sqlite.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <catch2/catch_test_macros.hpp>

namespace {

    TEST_CASE("database_exec_and_statement_round_trip_values") {
        const auto db_path = orangutan::testing::unique_test_db_path("sqlite", "sqlite.db");
        orangutan::sqlite::Database db(db_path);
        db.exec("CREATE TABLE sample (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL NOT NULL);", "create table failed");

        orangutan::sqlite::Statement insert(db, "INSERT INTO sample (name, score) VALUES (?, ?)");
        insert.bind_text(1, "alice");
        insert.bind_double(2, 9.5);
        static_cast<void>(insert.step());

        orangutan::sqlite::Statement query(db, "SELECT name, score FROM sample LIMIT 1");
        INFO("expected sqlite query to return one row");
        REQUIRE(query.step());
        CHECK(query.column_text(0) == "alice");
        CHECK(query.column_double(1) == 9.5);

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("transaction_rolls_back_when_not_committed") {
        const auto db_path = orangutan::testing::unique_test_db_path("sqlite-rollback", "sqlite.db");
        orangutan::sqlite::Database db(db_path);
        db.exec("CREATE TABLE sample (value TEXT NOT NULL);", "create table failed");

        {
            orangutan::sqlite::Transaction tx(db);
            orangutan::sqlite::Statement insert(db, "INSERT INTO sample (value) VALUES (?)");
            insert.bind_text(1, "transient");
            static_cast<void>(insert.step());
        }

        orangutan::sqlite::Statement query(db, "SELECT COUNT(*) FROM sample");
        INFO("expected sqlite count query to return one row");
        REQUIRE(query.step());
        CHECK(query.column_int(0) == 0);

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("database_configures_busy_timeout") {
        const auto db_path = orangutan::testing::unique_test_db_path("sqlite-timeout", "sqlite.db");
        orangutan::sqlite::Database db(db_path);

        orangutan::sqlite::Statement query(db, "PRAGMA busy_timeout");
        INFO("expected PRAGMA busy_timeout to return one row");
        REQUIRE(query.step());
        CHECK(query.column_int(0) == 1000);

        std::filesystem::remove_all(db_path.parent_path());
    };

} // namespace
