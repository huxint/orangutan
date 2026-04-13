#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "storage/sqlite.hpp"
#include "test-helpers.hpp"

struct SampleRow {
    std::string name;
    orangutan::base::i64 created_at;
    std::optional<std::string> note;
};

namespace orangutan::sqlite {

    template <>
    struct RowMapper<SampleRow> {
        static auto map(const Row &row) -> SampleRow {
            return SampleRow{
                .name = row.get<std::string>(0),
                .created_at = row.get<orangutan::base::i64>(1),
                .note = row.get<std::optional<std::string>>(2),
            };
        }
    };

} // namespace orangutan::sqlite

namespace {

    struct ScopedDbPath {
        explicit ScopedDbPath(std::string_view prefix, std::string_view filename = "sqlite.db")
        : path(orangutan::testing::unique_test_db_path(prefix, filename)) {}

        ~ScopedDbPath() {
            std::filesystem::remove_all(path.parent_path());
        }

        ScopedDbPath(const ScopedDbPath &) = delete;
        ScopedDbPath &operator=(const ScopedDbPath &) = delete;
        ScopedDbPath(ScopedDbPath &&) = delete;
        ScopedDbPath &operator=(ScopedDbPath &&) = delete;

        std::filesystem::path path;
    };

    TEST_CASE("database_read_side_operations_are_logically_const", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-const");
        const orangutan::sqlite::Database db(db_path.path);

        db.exec_script("CREATE TABLE sample (id INTEGER PRIMARY KEY, value TEXT NOT NULL);", "create sample");
        CHECK(db.try_exec_script("CREATE INDEX idx_sample_value ON sample(value);"));
        static_cast<void>(db.query("SELECT COUNT(*) FROM sample"));
        static_cast<void>(db.exec("INSERT INTO sample (value) VALUES (?)"));
        CHECK(db.handle() != nullptr);
        CHECK(db.changes() == 0);
    }

    static_assert(
        !std::is_invocable_v<decltype(&orangutan::sqlite::Database::transaction<void (*)(orangutan::sqlite::Database &)>),
                             const orangutan::sqlite::Database *,
                             void (*)(orangutan::sqlite::Database &)>);

    TEST_CASE("exec_script runs multi-statement schema setup", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-script");
        orangutan::sqlite::Database db(db_path.path);

        db.exec_script(R"(
            CREATE TABLE sample (id INTEGER PRIMARY KEY, value TEXT NOT NULL);
            CREATE INDEX idx_sample_value ON sample(value);
        )",
                       "create sample schema");

        CHECK(db.query("SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'idx_sample_value' LIMIT 1")
                  .optional<int>()
                  .has_value());
    }

    TEST_CASE("try_exec_script reports best-effort setup success and failure", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-try-script");
        orangutan::sqlite::Database db(db_path.path);

        CHECK(db.try_exec_script("CREATE TABLE best_effort (id INTEGER PRIMARY KEY);"));
        CHECK_FALSE(db.try_exec_script("CREATE TABL definitely_invalid_sql"));
    }

    TEST_CASE("database_constructor_creates_missing_parent_directories", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-parent", "nested/dir/sqlite.db");
        const auto parent = db_path.path.parent_path();
        std::filesystem::remove_all(parent);

        {
            orangutan::sqlite::Database db(db_path.path);
            CHECK(std::filesystem::exists(parent));
            db.exec_script("CREATE TABLE sample (id INTEGER PRIMARY KEY);", "create sample");
        }

        std::filesystem::remove_all(parent);
    }

    TEST_CASE("command_and_query_chain_round_trip_values", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-chain");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL NOT NULL, enabled INTEGER NOT NULL);",
                       "create sample");

        db.exec("INSERT INTO sample (name, score, enabled) VALUES (?, ?, ?)")
            .bind("alice", 9.5, true)
            .run();

        const auto row = db.query("SELECT name, score, enabled FROM sample LIMIT 1")
                             .one<std::tuple<std::string, orangutan::base::f64, bool>>();

        CHECK(std::get<0>(row) == "alice");
        CHECK(std::get<1>(row) == 9.5);
        CHECK(std::get<2>(row));
    }

    TEST_CASE("command_chain_reports_changes_after_run", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-single-use");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        db.exec("INSERT INTO sample (value) VALUES (?)").bind("once").run();
        CHECK(db.changes() == 1);
    }

    TEST_CASE("command_and_query_reject_double_bind_and_reuse", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-bind-reuse");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT NOT NULL); INSERT INTO sample(value) VALUES ('x');", "seed sample");

        auto command = db.exec("INSERT INTO sample (value) VALUES (?)");
        command.bind("first");
        CHECK_THROWS(command.bind("second"));
        command.run();
        CHECK_THROWS(command.run());

        auto query = db.query("SELECT value FROM sample WHERE value = ?");
        query.bind("x");
        CHECK_THROWS(query.bind("x"));
        static_cast<void>(query.one<std::string>());
        CHECK_THROWS(query.one<std::string>());
    }

    TEST_CASE("one_optional_and_mapping_failures_are_strict", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-failures");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT, extra INTEGER); INSERT INTO sample(value, extra) VALUES (NULL, 1), ('x', 2);", "seed sample");

        CHECK_FALSE(db.query("SELECT value FROM sample WHERE extra = 999").optional<std::string>().has_value());
        CHECK_THROWS_WITH(db.query("SELECT value FROM sample WHERE extra = 999").one<std::string>(),
                          Catch::Matchers::ContainsSubstring("expected one row, got none"));
        CHECK_THROWS_WITH(db.query("SELECT value FROM sample").one<std::string>(),
                          Catch::Matchers::ContainsSubstring("expected one row, got multiple"));
        CHECK_THROWS_WITH(db.query("SELECT value FROM sample").optional<std::string>(),
                          Catch::Matchers::ContainsSubstring("expected one row, got multiple"));
    }

    TEST_CASE("mapping_failures_are_strict", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-mapping-failures");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT, extra INTEGER); INSERT INTO sample(value, extra) VALUES (NULL, 1), ('x', 2);", "seed sample");

        CHECK_THROWS_WITH(db.query("SELECT value, extra FROM sample WHERE extra = 2").one<std::string>(),
                          Catch::Matchers::ContainsSubstring("expected one column"));
        CHECK_THROWS_WITH(db.query("SELECT value FROM sample WHERE extra = 1").one<std::string>(),
                          Catch::Matchers::ContainsSubstring("null"));
    }

    TEST_CASE("execution_mode_misuse_is_rejected", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-exec-mode");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT, extra INTEGER); INSERT INTO sample(value, extra) VALUES ('x', 2);", "seed sample");

        CHECK_THROWS_WITH(db.exec("SELECT value FROM sample").run(), Catch::Matchers::ContainsSubstring("result columns"));
        CHECK_THROWS_WITH(db.query("DELETE FROM sample WHERE extra = 2").one<int>(), Catch::Matchers::ContainsSubstring("zero result columns"));
    }

    TEST_CASE("placeholder_rules_match_spec", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-placeholders");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (left_value TEXT, right_value TEXT);", "create sample");

        db.exec("INSERT INTO sample (left_value, right_value) VALUES (?1, ?1)").bind("same").run();

        const auto duplicated = db.query("SELECT left_value, right_value FROM sample LIMIT 1").one<std::tuple<std::string, std::string>>();
        CHECK(std::get<0>(duplicated) == "same");
        CHECK(std::get<1>(duplicated) == "same");

        db.exec("INSERT INTO sample (left_value, right_value) VALUES (?2, ?1)").bind("right", "left").run();

        const auto reversed = db.query("SELECT left_value, right_value FROM sample WHERE rowid = 2").one<std::tuple<std::string, std::string>>();
        CHECK(std::get<0>(reversed) == "left");
        CHECK(std::get<1>(reversed) == "right");

        CHECK_THROWS_WITH(db.exec("INSERT INTO sample (left_value, right_value) VALUES (?, ?)").bind("only-one").run(),
                          Catch::Matchers::ContainsSubstring("bind"));
        CHECK_THROWS_WITH(db.exec("INSERT INTO sample (left_value, right_value) VALUES (?, ?1)").bind("a", "b").run(),
                          Catch::Matchers::ContainsSubstring("mixed"));
        CHECK_THROWS_WITH(db.exec("INSERT INTO sample (left_value, right_value) VALUES (?1, ?3)").bind("a", "b", "c").run(),
                          Catch::Matchers::ContainsSubstring("contiguous"));
    }

    TEST_CASE("statement_bind_all_matches_high_level_placeholder_rules", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-statement-bind-all");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (left_value TEXT, right_value TEXT);", "create sample");

        orangutan::sqlite::Statement mixed(db, "INSERT INTO sample (left_value, right_value) VALUES (?, ?1)");
        CHECK_THROWS_WITH(mixed.bind_all("a", "b"), Catch::Matchers::ContainsSubstring("mixed"));

        orangutan::sqlite::Statement sparse(db, "INSERT INTO sample (left_value, right_value) VALUES (?1, ?3)");
        CHECK_THROWS_WITH(sparse.bind_all("a", "b", "c"), Catch::Matchers::ContainsSubstring("contiguous"));
    }

    TEST_CASE("statement_bind_by_index_supports_sparse_indexed_placeholders", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-statement-sparse");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (left_value TEXT, right_value TEXT);", "create sample");

        orangutan::sqlite::Statement insert(db, "INSERT INTO sample (left_value, right_value) VALUES (?1, ?3)");
        insert.bind(1, "left");
        insert.bind(3, "right");
        static_cast<void>(insert.step());

        const auto row = db.query("SELECT left_value, right_value FROM sample LIMIT 1").one<std::tuple<std::string, std::string>>();
        CHECK(std::get<0>(row) == "left");
        CHECK(std::get<1>(row) == "right");
    }

    TEST_CASE("prepared_execution_rejects_trailing_sql", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-trailing-sql");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        CHECK_THROWS_WITH(db.exec(R"(INSERT INTO sample(value) VALUES ('x'); SELECT 1)"), Catch::Matchers::ContainsSubstring("trailing SQL"));
        CHECK_THROWS_WITH(db.query(R"(SELECT 1; SELECT 2)"), Catch::Matchers::ContainsSubstring("trailing SQL"));
        CHECK_THROWS_WITH(orangutan::sqlite::Statement(db, R"(SELECT 1; SELECT 2)"), Catch::Matchers::ContainsSubstring("trailing SQL"));
    }

    TEST_CASE("optional_binding_maps_empty_optional_to_null", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-optional-bind");
        orangutan::sqlite::Database db(db_path.path);

        std::optional<std::string> empty;
        const auto result = db.query("SELECT (?1 IS NULL), (?1 = '')").bind(empty).one<std::tuple<int, int>>();

        CHECK(std::get<0>(result) == 1);
        CHECK(std::get<1>(result) == 0);
    }

    TEST_CASE("row_mapping_collection_and_null_inputs_work", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-row-mapping");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (name TEXT NOT NULL, created_at INTEGER NOT NULL, note TEXT);", "create sample");

        db.exec("INSERT INTO sample (name, created_at, note) VALUES (?, ?, ?)").bind("alpha", static_cast<orangutan::base::i64>(7), nullptr).run();
        db.exec("INSERT INTO sample (name, created_at, note) VALUES (?, ?, ?)").bind("beta", static_cast<orangutan::base::i64>(8), std::string{"memo"}).run();

        const auto rows = db.query("SELECT name, created_at, note FROM sample ORDER BY created_at ASC").all<SampleRow>();
        REQUIRE(rows.size() == std::size_t{2});
        CHECK_FALSE(rows.front().note.has_value());
        CHECK(rows.back().note == std::optional<std::string>{"memo"});

        orangutan::sqlite::Statement statement(db, "SELECT name, created_at, note FROM sample ORDER BY created_at ASC LIMIT 1");
        REQUIRE(statement.step());
        const auto row = statement.row();
        CHECK(row.columns() == 3);
        CHECK(row.is_null(2));
    }

    TEST_CASE("for_each_handles_zero_rows_and_callback_exceptions", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-for-each");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT NOT NULL); INSERT INTO sample(value) VALUES ('x');", "seed sample");

        int zero_row_calls = 0;
        db.query("SELECT value FROM sample WHERE value = ?")
            .bind("missing")
            .for_each([&](const orangutan::sqlite::Row &) {
                ++zero_row_calls;
            });
        CHECK(zero_row_calls == 0);

        CHECK_THROWS_WITH(db.query("SELECT value FROM sample").for_each([&](const orangutan::sqlite::Row &) { throw std::runtime_error("callback boom"); }),
                          Catch::Matchers::ContainsSubstring("callback boom"));
    }

    TEST_CASE("row_view_reports_nullability_and_column_count", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-row-view");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (name TEXT NOT NULL, note TEXT); INSERT INTO sample(name, note) VALUES ('alpha', NULL);", "seed sample");

        orangutan::sqlite::Statement statement(db, "SELECT name, note FROM sample LIMIT 1");
        REQUIRE(statement.step());
        const auto row = statement.row();
        CHECK(row.columns() == 2);
        CHECK_FALSE(row.is_null(0));
        CHECK(row.is_null(1));
    }

    TEST_CASE("database_transaction_commits_and_returns_value", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-transaction-commit");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        const auto count = db.transaction([&](orangutan::sqlite::Database &tx) {
            tx.exec("INSERT INTO sample (value) VALUES (?)").bind("persisted").run();
            return tx.query("SELECT COUNT(*) FROM sample").one<int>();
        });

        CHECK(count == 1);
    }

    TEST_CASE("database_transaction_rolls_back_on_exception", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-transaction-rollback");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        CHECK_THROWS(db.transaction([&](orangutan::sqlite::Database &tx) {
            tx.exec("INSERT INTO sample (value) VALUES (?)").bind("rolled-back").run();
            throw std::runtime_error("rollback");
            return 0;
        }));

        CHECK(db.query("SELECT COUNT(*) FROM sample").one<int>() == 0);
    }

    TEST_CASE("transaction_guards_are_shared_across_high_and_low_level_apis", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-transaction-guards");
        orangutan::sqlite::Database db(db_path.path);

        CHECK_THROWS(db.transaction([&](orangutan::sqlite::Database &tx) {
            tx.transaction([](auto &) {});
        }));

        {
            orangutan::sqlite::Transaction tx(db);
            CHECK_THROWS(db.transaction([](auto &) {}));
        }

        CHECK_THROWS([&]() {
            orangutan::sqlite::Transaction outer(db);
            orangutan::sqlite::Transaction inner(db);
        }());
    }

    TEST_CASE("statement_reset_and_rebind_support_reuse", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-statement-reuse");
        orangutan::sqlite::Database db(db_path.path);
        db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        orangutan::sqlite::Statement insert(db, "INSERT INTO sample (value) VALUES (?1)");
        insert.bind(1, "second");
        static_cast<void>(insert.step());
        insert.reset();
        insert.clear_bindings();
        insert.bind_all("third");
        static_cast<void>(insert.step());

        const auto values = db.query("SELECT value FROM sample ORDER BY rowid ASC").all<std::string>();
        CHECK(values == std::vector<std::string>{"second", "third"});
    }

} // namespace
