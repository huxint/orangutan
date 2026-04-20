#include <concepts>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "storage/sqlite-throwing.hpp"
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
        static auto map(const Row &row) -> SqliteResult<SampleRow> {
            auto columns = read_columns<std::string, orangutan::base::i64, std::optional<std::string>>(row);
            if (!columns) {
                return std::unexpected(columns.error());
            }
            auto &[name, created_at, note] = *columns;
            return SampleRow{
                .name = std::move(name),
                .created_at = created_at,
                .note = std::move(note),
            };
        }
    };

} // namespace orangutan::sqlite

namespace {

    using orangutan::sqlite::open_or_throw;
    using orangutan::sqlite::prepare_or_throw;
    using orangutan::sqlite::exec_bind;
    using orangutan::sqlite::exec_script;

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
        const auto db = open_or_throw(db_path.path);

        exec_script(db, "CREATE TABLE sample (id INTEGER PRIMARY KEY, value TEXT NOT NULL);", "create sample");
        CHECK(db.try_exec_script("CREATE INDEX idx_sample_value ON sample(value);"));
        CHECK(db.query("SELECT COUNT(*) FROM sample").has_value());
        CHECK(db.exec("INSERT INTO sample (value) VALUES (?)").has_value());
        CHECK(db.handle() != nullptr);
        CHECK(db.changes() == 0);
    }

    static_assert(
        !std::invocable<decltype(&orangutan::sqlite::Database::transaction<void (*)(orangutan::sqlite::Database &)>),
                        const orangutan::sqlite::Database *,
                        void (*)(orangutan::sqlite::Database &)>);

    TEST_CASE("exec_script runs multi-statement schema setup", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-script");
        auto db = open_or_throw(db_path.path);

        exec_script(db, R"(
            CREATE TABLE sample (id INTEGER PRIMARY KEY, value TEXT NOT NULL);
            CREATE INDEX idx_sample_value ON sample(value);
        )",
                    "create sample schema");

        const auto found = orangutan::sqlite::query_optional<int>(
            db, "SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'idx_sample_value' LIMIT 1");
        CHECK(found.has_value());
    }

    TEST_CASE("try_exec_script reports best-effort setup success and failure", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-try-script");
        auto db = open_or_throw(db_path.path);

        CHECK(db.try_exec_script("CREATE TABLE best_effort (id INTEGER PRIMARY KEY);"));
        CHECK_FALSE(db.try_exec_script("CREATE TABL definitely_invalid_sql"));
    }

    TEST_CASE("database_factory_creates_missing_parent_directories", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-parent", "nested/dir/sqlite.db");
        const auto parent = db_path.path.parent_path();
        std::filesystem::remove_all(parent);

        {
            auto db = open_or_throw(db_path.path);
            CHECK(std::filesystem::exists(parent));
            exec_script(db, "CREATE TABLE sample (id INTEGER PRIMARY KEY);", "create sample");
        }

        std::filesystem::remove_all(parent);
    }

    TEST_CASE("command_and_query_chain_round_trip_values", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-chain");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL NOT NULL, enabled INTEGER NOT NULL);",
                    "create sample");

        exec_bind(db, "INSERT INTO sample (name, score, enabled) VALUES (?, ?, ?)", "alice", 9.5, true);

        const auto row = orangutan::sqlite::query_one<std::tuple<std::string, orangutan::base::f64, bool>>(
            db, "SELECT name, score, enabled FROM sample LIMIT 1");

        CHECK(std::get<0>(row) == "alice");
        CHECK(std::get<1>(row) == 9.5);
        CHECK(std::get<2>(row));
    }

    TEST_CASE("command_chain_reports_changes_after_run", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-single-use");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        exec_bind(db, "INSERT INTO sample (value) VALUES (?)", "once");
        CHECK(db.changes() == 1);
    }

    TEST_CASE("command_and_query_reject_double_bind_and_reuse", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-bind-reuse");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT NOT NULL); INSERT INTO sample(value) VALUES ('x');", "seed sample");

        {
            auto command_or = db.exec("INSERT INTO sample (value) VALUES (?)");
            REQUIRE(command_or.has_value());
            auto &command = *command_or;
            command.bind("first");
            // Second bind latches a state_error; surfaces at run()
            command.bind("second");
            const auto run1 = command.run();
            CHECK_FALSE(run1.has_value());
            CHECK_THAT(run1.error().message, Catch::Matchers::ContainsSubstring("already bound"));
        }

        {
            auto command_or = db.exec("INSERT INTO sample (value) VALUES (?)");
            REQUIRE(command_or.has_value());
            auto &command = *command_or;
            command.bind("fresh");
            CHECK(command.run().has_value());
            // Re-running an already consumed command fails.
            const auto run2 = command.run();
            CHECK_FALSE(run2.has_value());
            CHECK_THAT(run2.error().message, Catch::Matchers::ContainsSubstring("already consumed"));
        }

        {
            auto query_or = db.query("SELECT value FROM sample WHERE value = ?");
            REQUIRE(query_or.has_value());
            auto &query = *query_or;
            query.bind("x");
            query.bind("x");
            const auto first = query.one<std::string>();
            CHECK_FALSE(first.has_value());
            CHECK_THAT(first.error().message, Catch::Matchers::ContainsSubstring("already bound"));
        }
    }

    TEST_CASE("one_optional_row_count_mismatches_are_strict", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-failures");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT, extra INTEGER); INSERT INTO sample(value, extra) VALUES (NULL, 1), ('x', 2);", "seed sample");

        const auto missing = orangutan::sqlite::query_optional<std::string>(db, "SELECT value FROM sample WHERE extra = 999");
        CHECK_FALSE(missing.has_value());

        auto q_none = *db.query("SELECT value FROM sample WHERE extra = 999");
        const auto none_result = q_none.one<std::string>();
        CHECK_FALSE(none_result.has_value());
        CHECK_THAT(none_result.error().message, Catch::Matchers::ContainsSubstring("expected one row, got none"));

        auto q_multi = *db.query("SELECT value FROM sample");
        const auto multi_one = q_multi.one<std::string>();
        CHECK_FALSE(multi_one.has_value());
        CHECK_THAT(multi_one.error().message, Catch::Matchers::ContainsSubstring("expected one row, got multiple"));

        auto q_multi_opt = *db.query("SELECT value FROM sample");
        const auto multi_opt = q_multi_opt.optional<std::string>();
        CHECK_FALSE(multi_opt.has_value());
        CHECK_THAT(multi_opt.error().message, Catch::Matchers::ContainsSubstring("expected one row, got multiple"));
    }

    TEST_CASE("one maps returning rows without re-executing the statement", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-one-returning");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        const auto rowid = orangutan::sqlite::query_one<orangutan::base::i64>(
            db, "INSERT INTO sample (value) VALUES ('one') RETURNING rowid");

        CHECK(rowid == 1);
        CHECK(orangutan::sqlite::query_one<int>(db, "SELECT COUNT(*) FROM sample") == 1);
        CHECK(orangutan::sqlite::query_one<std::string>(db, "SELECT value FROM sample LIMIT 1") == "one");
    }

    TEST_CASE("optional maps returning rows without re-executing the statement", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-optional-returning");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        const auto rowid = orangutan::sqlite::query_optional<orangutan::base::i64>(
            db, "INSERT INTO sample (value) VALUES ('optional') RETURNING rowid");

        REQUIRE(rowid.has_value());
        CHECK(*rowid == 1);
        CHECK(orangutan::sqlite::query_one<int>(db, "SELECT COUNT(*) FROM sample") == 1);
        CHECK(orangutan::sqlite::query_one<std::string>(db, "SELECT value FROM sample LIMIT 1") == "optional");
    }

    TEST_CASE("mapping_failures_are_strict", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-mapping-failures");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT, extra INTEGER); INSERT INTO sample(value, extra) VALUES (NULL, 1), ('x', 2);", "seed sample");

        auto q_columns = *db.query("SELECT value, extra FROM sample WHERE extra = 2");
        const auto too_many_cols = q_columns.one<std::string>();
        CHECK_FALSE(too_many_cols.has_value());
        CHECK_THAT(too_many_cols.error().message, Catch::Matchers::ContainsSubstring("expected one column"));

        auto q_null = *db.query("SELECT value FROM sample WHERE extra = 1");
        const auto null_result = q_null.one<std::string>();
        CHECK_FALSE(null_result.has_value());
        CHECK_THAT(null_result.error().message, Catch::Matchers::ContainsSubstring("null"));
    }

    TEST_CASE("execution_mode_misuse_is_rejected", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-exec-mode");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT, extra INTEGER); INSERT INTO sample(value, extra) VALUES ('x', 2);", "seed sample");

        auto cmd_on_select = *db.exec("SELECT value FROM sample");
        const auto bad_run = cmd_on_select.run();
        CHECK_FALSE(bad_run.has_value());
        CHECK_THAT(bad_run.error().message, Catch::Matchers::ContainsSubstring("result columns"));

        auto query_on_delete = *db.query("DELETE FROM sample WHERE extra = 2");
        const auto bad_one = query_on_delete.one<int>();
        CHECK_FALSE(bad_one.has_value());
        CHECK_THAT(bad_one.error().message, Catch::Matchers::ContainsSubstring("zero result columns"));
    }

    TEST_CASE("placeholder_rules_match_spec", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-placeholders");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (left_value TEXT, right_value TEXT);", "create sample");

        exec_bind(db, "INSERT INTO sample (left_value, right_value) VALUES (?1, ?1)", "same");

        const auto duplicated = orangutan::sqlite::query_one<std::tuple<std::string, std::string>>(
            db, "SELECT left_value, right_value FROM sample LIMIT 1");
        CHECK(std::get<0>(duplicated) == "same");
        CHECK(std::get<1>(duplicated) == "same");

        exec_bind(db, "INSERT INTO sample (left_value, right_value) VALUES (?2, ?1)", "right", "left");

        const auto reversed = orangutan::sqlite::query_one<std::tuple<std::string, std::string>>(
            db, "SELECT left_value, right_value FROM sample WHERE rowid = 2");
        CHECK(std::get<0>(reversed) == "left");
        CHECK(std::get<1>(reversed) == "right");

        {
            auto cmd = *db.exec("INSERT INTO sample (left_value, right_value) VALUES (?, ?)");
            cmd.bind("only-one");
            const auto r = cmd.run();
            CHECK_FALSE(r.has_value());
            CHECK_THAT(r.error().message, Catch::Matchers::ContainsSubstring("bind"));
        }
        {
            auto cmd = *db.exec("INSERT INTO sample (left_value, right_value) VALUES (?, ?1)");
            cmd.bind("a", "b");
            const auto r = cmd.run();
            CHECK_FALSE(r.has_value());
            CHECK_THAT(r.error().message, Catch::Matchers::ContainsSubstring("mixed"));
        }
        {
            auto cmd = *db.exec("INSERT INTO sample (left_value, right_value) VALUES (?1, ?3)");
            cmd.bind("a", "b", "c");
            const auto r = cmd.run();
            CHECK_FALSE(r.has_value());
            CHECK_THAT(r.error().message, Catch::Matchers::ContainsSubstring("contiguous"));
        }
    }

    TEST_CASE("statement_bind_all_matches_high_level_placeholder_rules", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-statement-bind-all");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (left_value TEXT, right_value TEXT);", "create sample");

        {
            auto mixed = prepare_or_throw(db, "INSERT INTO sample (left_value, right_value) VALUES (?, ?1)");
            const auto bind_result = mixed.try_bind_all("a", "b");
            CHECK_FALSE(bind_result.has_value());
            CHECK_THAT(bind_result.error().message, Catch::Matchers::ContainsSubstring("mixed"));
        }

        {
            auto sparse = prepare_or_throw(db, "INSERT INTO sample (left_value, right_value) VALUES (?1, ?3)");
            const auto bind_result = sparse.try_bind_all("a", "b", "c");
            CHECK_FALSE(bind_result.has_value());
            CHECK_THAT(bind_result.error().message, Catch::Matchers::ContainsSubstring("contiguous"));
        }
    }

    TEST_CASE("statement_bind_by_index_supports_sparse_indexed_placeholders", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-statement-sparse");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (left_value TEXT, right_value TEXT);", "create sample");

        auto insert = prepare_or_throw(db, "INSERT INTO sample (left_value, right_value) VALUES (?1, ?3)");
        insert.bind(1, "left");
        insert.bind(3, "right");
        CHECK(insert.step().has_value());

        const auto row = orangutan::sqlite::query_one<std::tuple<std::string, std::string>>(
            db, "SELECT left_value, right_value FROM sample LIMIT 1");
        CHECK(std::get<0>(row) == "left");
        CHECK(std::get<1>(row) == "right");
    }

    TEST_CASE("prepared_execution_rejects_trailing_sql", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-trailing-sql");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        const auto exec_tail = db.exec(R"(INSERT INTO sample(value) VALUES ('x'); SELECT 1)");
        CHECK_FALSE(exec_tail.has_value());
        CHECK_THAT(exec_tail.error().message, Catch::Matchers::ContainsSubstring("trailing sql"));

        const auto query_tail = db.query(R"(SELECT 1; SELECT 2)");
        CHECK_FALSE(query_tail.has_value());
        CHECK_THAT(query_tail.error().message, Catch::Matchers::ContainsSubstring("trailing sql"));

        const auto stmt_tail = orangutan::sqlite::Statement::create(db, R"(SELECT 1; SELECT 2)");
        CHECK_FALSE(stmt_tail.has_value());
        CHECK_THAT(stmt_tail.error().message, Catch::Matchers::ContainsSubstring("trailing sql"));
    }

    TEST_CASE("optional_binding_maps_empty_optional_to_null", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-optional-bind");
        auto db = open_or_throw(db_path.path);

        std::optional<std::string> empty;
        const auto result = orangutan::sqlite::query_one<std::tuple<int, int>>(
            db, "SELECT (?1 IS NULL), COALESCE((?1 = ''), 0)", empty);

        CHECK(std::get<0>(result) == 1);
        CHECK(std::get<1>(result) == 0);
    }

    TEST_CASE("row_mapping_collection_and_null_inputs_work", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-row-mapping");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (name TEXT NOT NULL, created_at INTEGER NOT NULL, note TEXT);", "create sample");

        exec_bind(db, "INSERT INTO sample (name, created_at, note) VALUES (?, ?, ?)", "alpha", static_cast<orangutan::base::i64>(7), nullptr);
        exec_bind(db, "INSERT INTO sample (name, created_at, note) VALUES (?, ?, ?)", "beta", static_cast<orangutan::base::i64>(8), std::string{"memo"});

        const auto rows = orangutan::sqlite::query_all<SampleRow>(db, "SELECT name, created_at, note FROM sample ORDER BY created_at ASC");
        REQUIRE(rows.size() == std::size_t{2});
        CHECK_FALSE(rows.front().note.has_value());
        CHECK(rows.back().note == std::optional<std::string>{"memo"});

        auto statement = prepare_or_throw(db, "SELECT name, created_at, note FROM sample ORDER BY created_at ASC LIMIT 1");
        REQUIRE(orangutan::sqlite::unwrap(statement.step()));
        const auto row = orangutan::sqlite::unwrap(statement.row());
        CHECK(row.columns() == 3);
        CHECK(orangutan::sqlite::unwrap(row.is_null(2)));
    }

    TEST_CASE("read_columns unpacks rows and enforces column count", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-read-columns");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (name TEXT NOT NULL, created_at INTEGER NOT NULL, note TEXT);", "create sample");
        exec_bind(db, "INSERT INTO sample (name, created_at, note) VALUES (?, ?, ?)", "gamma", static_cast<orangutan::base::i64>(42), std::string{"hello"});

        auto statement = prepare_or_throw(db, "SELECT name, created_at, note FROM sample LIMIT 1");
        REQUIRE(orangutan::sqlite::unwrap(statement.step()));
        const auto row = orangutan::sqlite::unwrap(statement.row());

        const auto columns =
            orangutan::sqlite::read_columns<std::string, orangutan::base::i64, std::optional<std::string>>(row);
        REQUIRE(columns.has_value());
        const auto &[name, created_at, note] = *columns;
        CHECK(name == "gamma");
        CHECK(created_at == 42);
        CHECK(note == std::optional<std::string>{"hello"});

        const auto mismatch = orangutan::sqlite::read_columns<std::string, orangutan::base::i64>(row);
        REQUIRE_FALSE(mismatch.has_value());
        CHECK(mismatch.error().kind == orangutan::sqlite::sqlite_error_kind::mapping_error);
    }

    TEST_CASE("for_each_handles_zero_rows", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-for-each");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT NOT NULL); INSERT INTO sample(value) VALUES ('x');", "seed sample");

        int zero_row_calls = 0;
        auto zero_q = *db.query("SELECT value FROM sample WHERE value = ?");
        CHECK(zero_q.bind("missing").for_each([&](const orangutan::sqlite::Row &) {
                      ++zero_row_calls;
                  })
                  .has_value());
        CHECK(zero_row_calls == 0);

        int hit_rows = 0;
        auto full_q = *db.query("SELECT value FROM sample");
        CHECK(full_q.for_each([&](const orangutan::sqlite::Row &) { ++hit_rows; }).has_value());
        CHECK(hit_rows == 1);
    }

    TEST_CASE("row_view_reports_nullability_and_column_count", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-row-view");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (name TEXT NOT NULL, note TEXT); INSERT INTO sample(name, note) VALUES ('alpha', NULL);", "seed sample");

        auto statement = prepare_or_throw(db, "SELECT name, note FROM sample LIMIT 1");
        REQUIRE(orangutan::sqlite::unwrap(statement.step()));
        const auto row = orangutan::sqlite::unwrap(statement.row());
        CHECK(row.columns() == 2);
        CHECK_FALSE(orangutan::sqlite::unwrap(row.is_null(0)));
        CHECK(orangutan::sqlite::unwrap(row.is_null(1)));
    }

    TEST_CASE("database_transaction_commits_and_returns_value", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-transaction-commit");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        const auto count = db.transaction([&](orangutan::sqlite::Database &tx) {
            exec_bind(tx, "INSERT INTO sample (value) VALUES (?)", "persisted");
            return orangutan::sqlite::query_one<int>(tx, "SELECT COUNT(*) FROM sample");
        });

        REQUIRE(count.has_value());
        CHECK(*count == 1);
    }

    TEST_CASE("database_transaction_rolls_back_on_body_exception", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-transaction-rollback");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        auto run_transaction = [&] {
            auto result = db.transaction([&](orangutan::sqlite::Database &tx) {
                exec_bind(tx, "INSERT INTO sample (value) VALUES (?)", "rolled-back");
                throw std::runtime_error("rollback");
                return 0;
            });
            static_cast<void>(result);
        };
        CHECK_THROWS(run_transaction());

        CHECK(orangutan::sqlite::query_one<int>(db, "SELECT COUNT(*) FROM sample") == 0);
    }

    TEST_CASE("transaction_guards_are_shared_across_high_and_low_level_apis", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-transaction-guards");
        auto db = open_or_throw(db_path.path);

        {
            const auto nested = db.transaction([&](orangutan::sqlite::Database &tx) {
                return tx.transaction([](auto &) { return 0; });
            });
            CHECK_FALSE(nested.has_value());
            CHECK_THAT(nested.error().message, Catch::Matchers::ContainsSubstring("transaction already active"));
        }

        {
            auto outer_tx = orangutan::sqlite::Transaction::create(db);
            REQUIRE(outer_tx.has_value());
            const auto nested = db.transaction([](auto &) { return 0; });
            CHECK_FALSE(nested.has_value());
            CHECK_THAT(nested.error().message, Catch::Matchers::ContainsSubstring("transaction already active"));
        }

        {
            auto outer = orangutan::sqlite::Transaction::create(db);
            REQUIRE(outer.has_value());
            const auto inner = orangutan::sqlite::Transaction::create(db);
            CHECK_FALSE(inner.has_value());
            CHECK_THAT(inner.error().message, Catch::Matchers::ContainsSubstring("transaction already active"));
        }
    }

    TEST_CASE("statement_reset_and_rebind_support_reuse", "[storage][sqlite]") {
        const ScopedDbPath db_path("sqlite-statement-reuse");
        auto db = open_or_throw(db_path.path);
        exec_script(db, "CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

        auto insert = prepare_or_throw(db, "INSERT INTO sample (value) VALUES (?1)");
        insert.bind(1, "second");
        CHECK(insert.step().has_value());
        CHECK(insert.reset().has_value());
        CHECK(insert.clear_bindings().has_value());
        insert.bind_all("third");
        CHECK(insert.step().has_value());

        const auto values = orangutan::sqlite::query_all<std::string>(db, "SELECT value FROM sample ORDER BY rowid ASC");
        CHECK(values == std::vector<std::string>{"second", "third"});
    }

} // namespace
