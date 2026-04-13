# SQLite Wrapper Modernization Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current thin SQLite wrapper with a modern two-level API, then migrate storage and swarm callers onto the new chainable and strongly typed interfaces without changing intended module-level behavior.

**Architecture:** Keep SQLite access SQL-centric and explicit. Build the new wrapper in `src/storage/sqlite.hpp` around `Database`, `Command`, `Query`, `Row`, `Statement`, and `Transaction`, then migrate callers in compatibility-preserving order: first lock wrapper semantics with tests, then implement the wrapper and low-level escape hatch, then migrate wrapper consumers, then migrate raw SQLite users in swarm, and finally remove the old cursor-first normal path and `src/storage/sqlite.cpp`.

**Tech Stack:** C++23, sqlite3, Catch2, spdlog, magic_enum, nlohmann_json, xmake

**Spec:** `docs/superpowers/specs/2026-04-12-sqlite-wrapper-modernization-design.md`

---

## Planned File Structure

### New Files

- None required. Keep the wrapper consolidated in `src/storage/sqlite.hpp` per project convention.

### Removed Files

- `src/storage/sqlite.cpp` - delete after all wrapper definitions move into `src/storage/sqlite.hpp` and all callers use the new API.

### Modified Files

- `docs/superpowers/specs/2026-04-12-sqlite-wrapper-modernization-design.md` - approved design record.
- `src/storage/sqlite.hpp` - add the new wrapper surface, move definitions inline, and retain the low-level escape hatch.
- `src/storage/session-store.cpp` - migrate CRUD, schema setup, and transactions onto the new wrapper.
- `src/automation/automation-store.cpp` - migrate indexed-placeholder CRUD and typed row loading.
- `src/memory/memory-store.cpp` - migrate list/search/stats/update flows and reusable statements.
- `src/memory/memory-search.cpp` - migrate typed record collection and reusable update statements.
- `src/memory/memory-schema.cpp` - migrate schema and FTS setup to `exec_script(...)` / `try_exec_script(...)`.
- `src/swarm/mailbox.cpp` - replace raw sqlite3 open, schema setup, and CRUD with the wrapper while preserving log-and-return behavior.
- `src/swarm/team-manager.cpp` - replace raw sqlite3 open, schema setup, and CRUD with the wrapper while preserving log-and-return behavior.
- `tests/storage/sqlite-test.cpp` - expand wrapper-focused coverage for the new API surface and failure contracts.
- `tests/storage/session-store-test.cpp` - keep storage regression coverage green after wrapper migration.
- `tests/automation/automation-store-test.cpp` - extend or adjust for new wrapper-backed automation behavior where needed.
- `tests/memory/memory-test.cpp` - keep memory behavior locked while storage internals change.
- `tests/swarm/mailbox-test.cpp` - lock mailbox behavior across wrapper migration.
- `tests/swarm/team-manager-test.cpp` - lock team-manager behavior across wrapper migration.

---

## Chunk 1: Wrapper Semantics Lock

### Task 1: Expand wrapper tests to define the new public contract before implementation

**Files:**
- Modify: `tests/storage/sqlite-test.cpp`

- [ ] **Step 1: Add a failing compile-time test that `Database` read-side operations remain logically const**

```cpp
TEST_CASE("database_read_side_operations_are_logically_const", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-const", "sqlite.db");
    const orangutan::sqlite::Database db(db_path);

    db.exec_script("CREATE TABLE sample (id INTEGER PRIMARY KEY, value TEXT NOT NULL);", "create sample");
    CHECK(db.try_exec_script("CREATE INDEX idx_sample_value ON sample(value);"));
    static_cast<void>(db.query("SELECT COUNT(*) FROM sample"));
    static_cast<void>(db.exec("INSERT INTO sample (value) VALUES (?)"));
    CHECK(db.handle() != nullptr);
    CHECK(db.changes() == 0);
}

static_assert(!std::is_invocable_v<decltype(&orangutan::sqlite::Database::transaction<void (*)(orangutan::sqlite::Database &)>), const orangutan::sqlite::Database *, void (*)(orangutan::sqlite::Database &)>);
```

- [ ] **Step 2: Add a failing test for immediate script execution replacing current `exec`/`try_exec` semantics**

```cpp
TEST_CASE("exec_script runs multi-statement schema setup", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-script", "sqlite.db");
    orangutan::sqlite::Database db(db_path);

    db.exec_script(R"(
        CREATE TABLE sample (id INTEGER PRIMARY KEY, value TEXT NOT NULL);
        CREATE INDEX idx_sample_value ON sample(value);
    )", "create sample schema");

    CHECK(db.query("SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'idx_sample_value' LIMIT 1")
              .optional<int>()
              .has_value());
}

TEST_CASE("try_exec_script reports best-effort setup success and failure", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-try-script", "sqlite.db");
    orangutan::sqlite::Database db(db_path);

    CHECK(db.try_exec_script("CREATE TABLE best_effort (id INTEGER PRIMARY KEY);"));
    CHECK_FALSE(db.try_exec_script("CREATE TABL definitely_invalid_sql"));
}

TEST_CASE("database_constructor_creates_missing_parent_directories", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-parent", "nested/dir/sqlite.db");
    const auto parent = db_path.parent_path();
    std::filesystem::remove_all(parent);

    {
        orangutan::sqlite::Database db(db_path);
        CHECK(std::filesystem::exists(parent));
        db.exec_script("CREATE TABLE sample (id INTEGER PRIMARY KEY);", "create sample");
    }

    std::filesystem::remove_all(parent);
}
```

- [ ] **Step 3: Add failing tests for chainable `Command` and `Query` happy paths**

```cpp
TEST_CASE("command_and_query_chain_round_trip_values", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-chain", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL NOT NULL, enabled INTEGER NOT NULL);", "create sample");

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
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-single-use", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

    db.exec("INSERT INTO sample (value) VALUES (?)").bind("once").run();
    CHECK(db.changes() == 1);
}
```

- [ ] **Step 4: Add a failing test that explicit double-bind and post-terminal reuse are rejected**

```cpp
TEST_CASE("command_and_query_reject_double_bind_and_reuse", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-bind-reuse", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
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
```

- [ ] **Step 5: Add a failing test that `optional<T>()` and `one<T>()` enforce row-count rules with row-count-specific errors**

```cpp
TEST_CASE("one_optional_and_mapping_failures_are_strict", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-failures", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (value TEXT, extra INTEGER); INSERT INTO sample(value, extra) VALUES (NULL, 1), ('x', 2);", "seed sample");

    CHECK_FALSE(db.query("SELECT value FROM sample WHERE extra = 999").optional<std::string>().has_value());
    CHECK_THROWS_WITH(db.query("SELECT value FROM sample WHERE extra = 999").one<std::string>(), Catch::Matchers::ContainsSubstring("expected one row, got none"));
    CHECK_THROWS_WITH(db.query("SELECT value FROM sample").one<std::string>(), Catch::Matchers::ContainsSubstring("expected one row, got multiple"));
    CHECK_THROWS_WITH(db.query("SELECT value FROM sample").optional<std::string>(), Catch::Matchers::ContainsSubstring("expected one row, got multiple"));
}
```

- [ ] **Step 6: Add a failing test that mapping strictness rejects tuple arity mismatches and null-to-non-optional extraction with explicit error categories**

```cpp
TEST_CASE("mapping_failures_are_strict", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-mapping-failures", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (value TEXT, extra INTEGER); INSERT INTO sample(value, extra) VALUES (NULL, 1), ('x', 2);", "seed sample");

    CHECK_THROWS_WITH(db.query("SELECT value, extra FROM sample WHERE extra = 2").one<std::string>(), Catch::Matchers::ContainsSubstring("expected one column"));
    CHECK_THROWS_WITH(db.query("SELECT value FROM sample WHERE extra = 1").one<std::string>(), Catch::Matchers::ContainsSubstring("null"));
}
```

- [ ] **Step 7: Add a failing test that wrong execution mode is rejected with an execution-mode-specific error**

```cpp
TEST_CASE("execution_mode_misuse_is_rejected", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-exec-mode", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (value TEXT, extra INTEGER); INSERT INTO sample(value, extra) VALUES ('x', 2);", "seed sample");

    CHECK_THROWS_WITH(db.exec("SELECT value FROM sample").run(), Catch::Matchers::ContainsSubstring("result columns"));
    CHECK_THROWS_WITH(db.query("DELETE FROM sample WHERE extra = 2").one<int>(), Catch::Matchers::ContainsSubstring("zero result columns"));
}
```

- [ ] **Step 8: Add a failing test for indexed placeholders, repeated `?1`, mixed placeholder rejection, sparse indexed rejection, and bind-count mismatch**

```cpp
TEST_CASE("placeholder_rules_match_spec", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-placeholders", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (left_value TEXT, right_value TEXT);", "create sample");

    db.exec("INSERT INTO sample (left_value, right_value) VALUES (?1, ?1)")
        .bind("same")
        .run();

    const auto duplicated = db.query("SELECT left_value, right_value FROM sample LIMIT 1")
        .one<std::tuple<std::string, std::string>>();
    CHECK(std::get<0>(duplicated) == "same");
    CHECK(std::get<1>(duplicated) == "same");

    db.exec("INSERT INTO sample (left_value, right_value) VALUES (?2, ?1)")
        .bind("right", "left")
        .run();

    const auto reversed = db.query("SELECT left_value, right_value FROM sample WHERE rowid = 2")
        .one<std::tuple<std::string, std::string>>();
    CHECK(std::get<0>(reversed) == "left");
    CHECK(std::get<1>(reversed) == "right");

    CHECK_THROWS_WITH(db.exec("INSERT INTO sample (left_value, right_value) VALUES (?, ?)").bind("only-one").run(), Catch::Matchers::ContainsSubstring("bind"));
    CHECK_THROWS_WITH(db.exec("INSERT INTO sample (left_value, right_value) VALUES (?, ?1)").bind("a", "b").run(), Catch::Matchers::ContainsSubstring("mixed"));
    CHECK_THROWS_WITH(db.exec("INSERT INTO sample (left_value, right_value) VALUES (?1, ?3)").bind("a", "b", "c").run(), Catch::Matchers::ContainsSubstring("contiguous"));
}
```

- [ ] **Step 9: Add a failing test that low-level `Statement::bind_all(...)` matches the same mixed and contiguous indexed-placeholder rules**

```cpp
TEST_CASE("statement_bind_all_matches_high_level_placeholder_rules", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-statement-bind-all", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (left_value TEXT, right_value TEXT);", "create sample");

    orangutan::sqlite::Statement mixed(db, "INSERT INTO sample (left_value, right_value) VALUES (?, ?1)");
    CHECK_THROWS_WITH(mixed.bind_all("a", "b"), Catch::Matchers::ContainsSubstring("mixed"));

    orangutan::sqlite::Statement sparse(db, "INSERT INTO sample (left_value, right_value) VALUES (?1, ?3)");
    CHECK_THROWS_WITH(sparse.bind_all("a", "b", "c"), Catch::Matchers::ContainsSubstring("contiguous"));
}
```

- [ ] **Step 10: Add a failing test that low-level `Statement::bind(index, value)` still supports sparse indexed placeholders explicitly**

```cpp
TEST_CASE("statement_bind_by_index_supports_sparse_indexed_placeholders", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-statement-sparse", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (left_value TEXT, right_value TEXT);", "create sample");

    orangutan::sqlite::Statement insert(db, "INSERT INTO sample (left_value, right_value) VALUES (?1, ?3)");
    insert.bind(1, "left");
    insert.bind(3, "right");
    static_cast<void>(insert.step());

    const auto row = db.query("SELECT left_value, right_value FROM sample LIMIT 1")
        .one<std::tuple<std::string, std::string>>();
    CHECK(std::get<0>(row) == "left");
    CHECK(std::get<1>(row) == "right");
}
```

- [ ] **Step 11: Add a failing test for prepared trailing-SQL rejection in `exec`, `query`, and `Statement`**

```cpp
TEST_CASE("prepared_execution_rejects_trailing_sql", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-trailing-sql", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

    CHECK_THROWS_WITH(db.exec(R"(INSERT INTO sample(value) VALUES ('x'); SELECT 1)"), Catch::Matchers::ContainsSubstring("trailing SQL"));
    CHECK_THROWS_WITH(db.query(R"(SELECT 1; SELECT 2)"), Catch::Matchers::ContainsSubstring("trailing SQL"));
    CHECK_THROWS_WITH(orangutan::sqlite::Statement(db, R"(SELECT 1; SELECT 2)"), Catch::Matchers::ContainsSubstring("trailing SQL"));
}
```

- [ ] **Step 12: Add a failing test that `std::optional<T>` binding maps empty optionals to SQL NULL**

```cpp
TEST_CASE("optional_binding_maps_empty_optional_to_null", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-optional-bind", "sqlite.db");
    orangutan::sqlite::Database db(db_path);

    std::optional<std::string> empty;
    const auto result = db.query("SELECT (?1 IS NULL), (?1 = '')")
        .bind(empty)
        .one<std::tuple<int, int>>();

    CHECK(std::get<0>(result) == 1);
    CHECK(std::get<1>(result) == 0);
}
```

- [ ] **Step 13: Add a failing test that `std::nullptr_t`, `base::i64`, `all<T>()`, and row-mapper struct support work together**

```cpp
struct SampleRow {
    std::string name;
    orangutan::base::i64 created_at;
    std::optional<std::string> note;
};

template<>
struct orangutan::sqlite::RowMapper<SampleRow> {
    static auto map(const orangutan::sqlite::Row &row) -> SampleRow {
        return SampleRow{
            .name = row.get<std::string>(0),
            .created_at = row.get<orangutan::base::i64>(1),
            .note = row.get<std::optional<std::string>>(2),
        };
    }
};

TEST_CASE("row_mapping_collection_and_null_inputs_work", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-row-mapping", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (name TEXT NOT NULL, created_at INTEGER NOT NULL, note TEXT);", "create sample");

    db.exec("INSERT INTO sample (name, created_at, note) VALUES (?, ?, ?)").bind("alpha", static_cast<orangutan::base::i64>(7), nullptr).run();
    db.exec("INSERT INTO sample (name, created_at, note) VALUES (?, ?, ?)").bind("beta", static_cast<orangutan::base::i64>(8), std::string{"memo"}).run();

    const auto rows = db.query("SELECT name, created_at, note FROM sample ORDER BY created_at ASC").all<SampleRow>();
    REQUIRE(rows.size() == 2UL);
    CHECK_FALSE(rows.front().note.has_value());
    CHECK(rows.back().note == std::optional<std::string>{"memo"});

    orangutan::sqlite::Statement statement(db, "SELECT name, created_at, note FROM sample ORDER BY created_at ASC LIMIT 1");
    REQUIRE(statement.step());
    const auto row = statement.row();
    CHECK(row.columns() == 3);
    CHECK(row.is_null(2));

}
```

- [ ] **Step 14: Add a failing test that `for_each(...)` is a no-op on zero rows and propagates callback exceptions**

```cpp
TEST_CASE("for_each_handles_zero_rows_and_callback_exceptions", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-for-each", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (value TEXT NOT NULL); INSERT INTO sample(value) VALUES ('x');", "seed sample");

    int zero_row_calls = 0;
    db.query("SELECT value FROM sample WHERE value = ?")
        .bind("missing")
        .for_each([&](const orangutan::sqlite::Row &) {
            ++zero_row_calls;
        });
    CHECK(zero_row_calls == 0);

    CHECK_THROWS_WITH(db.query("SELECT value FROM sample").for_each([&](const orangutan::sqlite::Row &) {
        throw std::runtime_error("callback boom");
    }), Catch::Matchers::ContainsSubstring("callback boom"));
}
```

- [ ] **Step 15: Add a failing test that low-level `Statement::row()`, `Row::is_null()`, and `Row::columns()` behave as specified**

```cpp
TEST_CASE("row_view_reports_nullability_and_column_count", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-row-view", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (name TEXT NOT NULL, note TEXT); INSERT INTO sample(name, note) VALUES ('alpha', NULL);", "seed sample");

    orangutan::sqlite::Statement statement(db, "SELECT name, note FROM sample LIMIT 1");
    REQUIRE(statement.step());
    const auto row = statement.row();
    CHECK(row.columns() == 2);
    CHECK_FALSE(row.is_null(0));
    CHECK(row.is_null(1));
}
```

- [ ] **Step 16: Add a failing test that `Database::transaction(...)` commits and propagates return values**

```cpp
TEST_CASE("database_transaction_commits_and_returns_value", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-transaction-commit", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

    const auto count = db.transaction([&](orangutan::sqlite::Database &tx) {
        tx.exec("INSERT INTO sample (value) VALUES (?)").bind("persisted").run();
        return tx.query("SELECT COUNT(*) FROM sample").one<int>();
    });

    CHECK(count == 1);
}
```

- [ ] **Step 17: Add a failing test that `Database::transaction(...)` rolls back on exception**

```cpp
TEST_CASE("database_transaction_rolls_back_on_exception", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-transaction-rollback", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

    CHECK_THROWS(db.transaction([&](orangutan::sqlite::Database &tx) {
        tx.exec("INSERT INTO sample (value) VALUES (?)").bind("rolled-back").run();
        throw std::runtime_error("rollback");
        return 0;
    }));

    CHECK(db.query("SELECT COUNT(*) FROM sample").one<int>() == 0);
}
```

- [ ] **Step 18: Add a failing test that `Transaction` and `Database::transaction(...)` share one-owner guards in both directions**

```cpp
TEST_CASE("transaction_guards_are_shared_across_high_and_low_level_apis", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-transaction-guards", "sqlite.db");
    orangutan::sqlite::Database db(db_path);

    CHECK_THROWS(db.transaction([&](orangutan::sqlite::Database &tx) {
        tx.transaction([](auto &) {});
    }));

    {
        orangutan::sqlite::Transaction tx(db);
        CHECK_THROWS(db.transaction([](auto &) {}));
    }

    CHECK_THROWS([
        &]() {
            orangutan::sqlite::Transaction outer(db);
            orangutan::sqlite::Transaction inner(db);
        }());
}
```

- [ ] **Step 19: Add a failing test that low-level `Statement` reset/clear-bindings/rebind reuse is observable in query results**

```cpp
TEST_CASE("statement_reset_and_rebind_support_reuse", "[storage][sqlite]") {
    const auto db_path = orangutan::testing::unique_test_db_path("sqlite-statement-reuse", "sqlite.db");
    orangutan::sqlite::Database db(db_path);
    db.exec_script("CREATE TABLE sample (value TEXT NOT NULL);", "create sample");

    orangutan::sqlite::Statement insert(db, "INSERT INTO sample (value) VALUES (?1)");
    insert.bind(1, "second");
    static_cast<void>(insert.step());
    insert.reset();
    insert.clear_bindings();
    insert.bind_all("third");
    static_cast<void>(insert.step());

    const auto values = db.query("SELECT value FROM sample ORDER BY rowid ASC")
        .all<std::string>();
    CHECK(values == std::vector<std::string>{"second", "third"});
}
```

- [ ] **Step 20: Run the narrow sqlite wrapper subset first, then the broader storage suite, to confirm failures are localized**

Run: `xmake run test-storage -- --reporter compact "[storage][sqlite]"`
Expected: FAIL due to missing wrapper symbols, compile errors tied to the new API surface, or the newly added sqlite wrapper assertions only.

Run: `xmake run test-storage`
Expected: FAIL only in sqlite wrapper-related build or test paths.

- [ ] **Step 21: Commit the contract-lock test expansion**

```bash
git add tests/storage/sqlite-test.cpp
git commit -m "test(storage): lock sqlite wrapper modernization contracts"
```

---

## Chunk 2: Wrapper Core Implementation

### Task 2: Replace the current thin wrapper with the new two-level API in `src/storage/sqlite.hpp`

**Files:**
- Modify: `src/storage/sqlite.hpp`
- Delete: `src/storage/sqlite.cpp`
- Test: `tests/storage/sqlite-test.cpp`

- [ ] **Step 1: Move current implementation into `src/storage/sqlite.hpp` as a starting point before changing behavior**

```cpp
// Inline the current Database / Statement / Transaction definitions into sqlite.hpp first.
// Keep behavior unchanged in this step except for file location.
```

- [ ] **Step 2: Add the new public types and base helpers with the smallest viable internal model**

```cpp
namespace orangutan::sqlite {

    class Row;
    template<typename T>
    struct RowMapper;

    class Command;
    class Query;

    class Database {
    public:
        explicit Database(const std::filesystem::path &path);
        ~Database();

        auto exec(std::string_view sql) const -> Command;
        auto query(std::string_view sql) const -> Query;
        void exec_script(std::string_view sql, std::string_view context) const;
        [[nodiscard]] auto try_exec_script(std::string_view sql) const -> bool;
        template<typename Fn>
        decltype(auto) transaction(Fn &&fn);
        [[nodiscard]] auto changes() const -> int;
        [[nodiscard]] auto handle() const noexcept -> sqlite3 *;
    private:
        sqlite3 *db_ = nullptr;
        bool transaction_active_ = false;
    };
}
```

- [ ] **Step 3: Implement prepared-SQL validation and execution-mode boundaries**

```cpp
// Validate that sqlite3_prepare_v2 consumes one statement with only trailing whitespace remaining.
// Throw on trailing non-whitespace SQL.
// Keep exec_script()/try_exec_script() on sqlite3_exec for multi-statement and setup flows.
```

- [ ] **Step 4: Implement generic bind and row extraction helpers for the supported first-wave types**

```cpp
// Support: std::string_view, std::string, const char*, nullptr, int, base::i64, std::int64_t, base::f64, bool, std::optional<T>
// Distinguish null const char* from text.
// Extract: std::string, int, base::i64/std::int64_t, base::f64, bool, std::optional<T>
```

- [ ] **Step 5: Implement `Row`, `RowMapper`, tuple/scalar mapping, and strict mapping failures**

```cpp
// Row::get<T>(index), Row::is_null(index), Row::columns()
// Tuple mapping via index_sequence.
// one/optional/all/for_each semantics with strict row-count and column-count checks.
```

- [ ] **Step 6: Implement `Command`, `Query`, and low-level `Statement` reuse semantics**

```cpp
class Statement {
public:
    template<typename T>
    auto bind(int index, T &&value) -> Statement &;

    template<typename... Args>
    auto bind_all(Args &&...args) -> Statement &;

    void reset();
    void clear_bindings();
    [[nodiscard]] auto step() -> bool;
    [[nodiscard]] auto row() const -> Row;
};
```

- [ ] **Step 7: Implement transaction guard semantics and preserve constructor behavior**

```cpp
// Preserve parent-directory creation and busy_timeout setup.
// Implement one-owner transaction guard across Transaction and Database::transaction(...).
// Keep wrapper itself unsynchronized.
```

- [ ] **Step 8: Run the wrapper tests and fix only wrapper code until they pass**

Run: `xmake run test-storage`
Expected: PASS for `tests/storage/sqlite-test.cpp` and no new regressions in the storage target.

- [ ] **Step 9: Commit the wrapper core**

```bash
git add src/storage/sqlite.hpp src/storage/sqlite.cpp tests/storage/sqlite-test.cpp
git commit -m "refactor(storage): modernize sqlite wrapper core"
```

---

## Chunk 3: Existing Wrapper Consumers

### Task 3: Migrate `session-store` to the new wrapper and keep storage behavior stable

**Files:**
- Modify: `src/storage/session-store.cpp`
- Test: `tests/storage/session-store-test.cpp`
- Test: `tests/storage/sqlite-test.cpp`

- [ ] **Step 1: Add a failing regression test for one representative `session-store` read path and one write path if coverage is missing**

```cpp
TEST_CASE("session_store_round_trips_messages_after_sqlite_wrapper_cutover", "[storage][session-store]") {
    // Use existing SessionStore test helpers and verify create/load still work after wrapper migration.
}
```

- [ ] **Step 2: Run the storage suite to confirm the new assertion fails only because `session-store` still uses removed wrapper methods**

Run: `xmake run test-storage`
Expected: FAIL only where `session-store` still depends on old wrapper APIs or the new assertion if added.

- [ ] **Step 3: Replace immediate schema/setup calls with `exec_script(...)` / `try_exec_script(...)`**

```cpp
db_.exec_script("PRAGMA foreign_keys = ON;", "Failed to enable SQLite foreign keys");
db_.exec_script(R"(... CREATE TABLE ...)", "Failed to create session schema");
```

- [ ] **Step 4: Replace existence and single-row checks with `query(...).optional<T>()` / `one<T>()`**

```cpp
return db.query("SELECT 1 FROM sessions WHERE id = ? LIMIT 1")
    .bind(session_id)
    .optional<int>()
    .has_value();
```

- [ ] **Step 5: Replace multi-row loads with tuple or explicit row mappers, keeping domain conversion local**

```cpp
for (const auto &[role_text, content_json] : db_.query("SELECT role, content_json FROM messages WHERE session_id = ? ORDER BY seq")
         .bind(session_id)
         .all<std::tuple<std::string, std::string>>()) {
    ...
}
```

- [ ] **Step 6: Keep reusable insert loops on low-level `Statement` where that remains the clearest path**

```cpp
sqlite::Statement insert_msg(db, "INSERT INTO messages (session_id, seq, role, content_json) VALUES (?, ?, ?, ?)");
for (...) {
    insert_msg.reset();
    insert_msg.clear_bindings();
    insert_msg.bind_all(session_id, static_cast<int>(index), role_text, content_json);
    static_cast<void>(insert_msg.step());
}
```

- [ ] **Step 7: Replace manual RAII transaction shells with `db.transaction(...)` where it simplifies the code and keeps semantics obvious**

```cpp
return db_.transaction([&](sqlite::Database &tx) {
    insert_session(tx, session_id, metadata);
    write_messages(tx, session_id, messages, 0);
    return session_id;
});
```

- [ ] **Step 8: Run the storage suite and fix only `session-store` migration regressions**

Run: `xmake run test-storage`
Expected: PASS

- [ ] **Step 9: Commit the `session-store` cutover**

```bash
git add src/storage/session-store.cpp tests/storage/session-store-test.cpp tests/storage/sqlite-test.cpp
git commit -m "refactor(storage): migrate session store to sqlite wrapper v2"
```


### Task 4: Migrate automation and memory wrapper consumers to the new API

**Files:**
- Modify: `src/automation/automation-store.cpp`
- Modify: `src/memory/memory-store.cpp`
- Modify: `src/memory/memory-search.cpp`
- Modify: `src/memory/memory-schema.cpp`
- Test: `tests/automation/automation-store-test.cpp`
- Test: `tests/memory/memory-test.cpp`

- [ ] **Step 1: Add a failing automation-store regression test for indexed placeholders if current coverage does not already lock it**

```cpp
TEST_CASE("automation_store_filters_with_indexed_placeholders_after_wrapper_cutover", "[automation][store]") {
    // Seed two agent keys and verify (?1 = '' OR agent_key = ?1) style queries still behave correctly.
}
```

- [ ] **Step 2: Add a failing memory regression test for FTS setup fallback if current coverage does not already lock it**

```cpp
TEST_CASE("memory_store_constructs_when_fts_setup_is_best_effort", "[memory][store]") {
    // Lock current non-throwing behavior when optional FTS setup is unavailable.
}
```

- [ ] **Step 3: Run `test-automation` and `test-memory` to confirm failures are localized to wrapper API removal or the new regression assertions**

Run: `xmake run test-automation`
Expected: FAIL only in automation-store code paths or new regression assertions.

Run: `xmake run test-memory`
Expected: FAIL only in memory storage code paths or new regression assertions.

- [ ] **Step 4: Migrate automation-store reads and writes using `query`, `exec`, and row mappers while preserving `?1` / `?2` SQL**

```cpp
return db_.query(R"(
    SELECT id, agent_key, name, enabled, schedule_kind, schedule_value, prompt, notes, delivery_json, last_run_at, last_status
    FROM tasks
    WHERE (?1 = '' OR agent_key = ?1) AND (id = ?2 OR name = ?2)
    LIMIT 1
)")
.bind(agent_key, id_or_name)
.optional<TaskSpec>();
```

- [ ] **Step 5: Migrate memory schema setup to `exec_script(...)` and FTS probing to `try_exec_script(...)`**

```cpp
db.exec_script(R"(... CREATE TABLE / CREATE INDEX ...)", "Failed to create memory schema");
if (!db.try_exec_script("CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5(...);") ) {
    return false;
}
```

- [ ] **Step 6: Keep reusable memory update loops on `Statement` where reuse is clearer than rebuilding chain objects**

```cpp
sqlite::Statement touch(db, "UPDATE memories SET access_count = access_count + 1, last_accessed_at = datetime('now') WHERE id = ?");
for (const auto &record : records) {
    touch.reset();
    touch.clear_bindings();
    touch.bind(1, record.id);
    static_cast<void>(touch.step());
}
```

- [ ] **Step 7: Run the automation and memory suites and fix only migration regressions**

Run: `xmake run test-automation`
Expected: PASS

Run: `xmake run test-memory`
Expected: PASS

- [ ] **Step 8: Commit the automation/memory cutover**

```bash
git add src/automation/automation-store.cpp src/memory/memory-store.cpp src/memory/memory-search.cpp src/memory/memory-schema.cpp tests/automation/automation-store-test.cpp tests/memory/memory-test.cpp
git commit -m "refactor(storage): migrate automation and memory sqlite callers"
```

---

## Chunk 4: Raw SQLite Swarm Migration And Cleanup

### Task 5: Replace raw sqlite3 usage in `mailbox` and `team-manager` while preserving module-level behavior

**Files:**
- Modify: `src/swarm/mailbox.cpp`
- Modify: `src/swarm/team-manager.cpp`
- Test: `tests/swarm/mailbox-test.cpp`
- Test: `tests/swarm/team-manager-test.cpp`

- [ ] **Step 1: Add failing regression tests for constructor setup and read/write behavior if current coverage is too shallow**

```cpp
TEST_CASE("AgentMailbox constructor initializes schema and send failures stay non-throwing", "[swarm][mailbox]") {
    orangutan::swarm::AgentMailbox mailbox(std::filesystem::path{":memory:"});
    mailbox.send("team1", "coordinator", "worker1", "hello");
    CHECK(mailbox.poll("team1", "worker1").size() == 1UL);
}

TEST_CASE("TeamManager preserves find/list fallback behavior after sqlite wrapper cutover", "[swarm][team-manager]") {
    orangutan::swarm::TeamManager manager(std::filesystem::path{":memory:"});
    auto team = manager.create_team("test-team", "desc", "lead");
    CHECK(manager.find_team(team.id).has_value());
}
```

- [ ] **Step 2: Run the swarm suite to confirm failures are localized to the migration targets or new assertions**

Run: `xmake run test-swarm`
Expected: FAIL only in mailbox/team-manager code paths or the new regression assertions.

- [ ] **Step 3: Introduce `sqlite::Database` into both modules and move constructor setup to wrapper-backed calls**

```cpp
struct AgentMailbox::Impl {
    sqlite::Database db;
    std::mutex mutex;

    explicit Impl(const std::filesystem::path &db_path)
    : db(db_path) {
        db.exec_script("PRAGMA journal_mode=WAL;", "Failed to enable mailbox WAL mode");
        db.exec_script("CREATE TABLE IF NOT EXISTS ...", "Failed to create mailbox table");
    }
};
```

- [ ] **Step 4: Replace CRUD operations with wrapper calls, but preserve current log-and-return module contracts by catching wrapper exceptions at the module boundary**

```cpp
try {
    impl_->db.exec("INSERT INTO agent_mailbox (...) VALUES (?, ?, ?, ?, ?, ?, 0, ?)")
        .bind(id, team_id, from, to, text, ts, type_str)
        .run();
} catch (const std::exception &ex) {
    spdlog::error("failed to insert mailbox message: {}", ex.what());
    return;
}
```

- [ ] **Step 5: Use explicit row mappers or tuples for mailbox/team-manager read paths, keeping public return types unchanged**

```cpp
const auto records = impl_->db.query("SELECT id, name, description, lead_agent_id, created_at, active FROM teams WHERE active = 1")
    .all<TeamRecord>();
```

- [ ] **Step 6: Run the swarm suite and fix only mailbox/team-manager migration regressions**

Run: `xmake run test-swarm`
Expected: PASS

- [ ] **Step 7: Commit the raw-SQLite cutover**

```bash
git add src/swarm/mailbox.cpp src/swarm/team-manager.cpp tests/swarm/mailbox-test.cpp tests/swarm/team-manager-test.cpp
git commit -m "refactor(swarm): migrate mailbox and team manager sqlite access"
```


### Task 6: Remove the old normal path and verify the whole sqlite migration surface

**Files:**
- Modify: `src/storage/sqlite.hpp`
- Delete: `src/storage/sqlite.cpp`
- Verify: `tests/storage/sqlite-test.cpp`
- Verify: `tests/storage/session-store-test.cpp`
- Verify: `tests/automation/automation-store-test.cpp`
- Verify: `tests/memory/memory-test.cpp`
- Verify: `tests/swarm/mailbox-test.cpp`
- Verify: `tests/swarm/team-manager-test.cpp`

- [ ] **Step 1: Delete legacy cursor-first normal-path methods once all callers are migrated**

```cpp
// Remove public bind_text/bind_int/bind_double/column_text/column_int/column_double from the normal wrapper API.
// Keep only the modernized low-level Statement surface.
```

- [ ] **Step 2: Delete `src/storage/sqlite.cpp` after all definitions live in `src/storage/sqlite.hpp`**

Run: `git diff -- src/storage/sqlite.hpp src/storage/sqlite.cpp`
Expected: the `.cpp` file is redundant and safe to remove.

- [ ] **Step 3: Run the focused sqlite migration suites**

Run: `xmake run test-storage`
Expected: PASS

Run: `xmake run test-automation`
Expected: PASS

Run: `xmake run test-memory`
Expected: PASS

Run: `xmake run test-swarm`
Expected: PASS

- [ ] **Step 4: If any suite fails, fix the smallest wrapper or caller issue and re-run only the affected suite before re-running the full focused set**

```cpp
// Do not widen the API to preserve removed methods.
// Fix the migrated caller or the wrapper contract instead.
```

- [ ] **Step 5: Commit the final cleanup**

```bash
git add src/storage/sqlite.hpp src/storage/sqlite.cpp src/storage/session-store.cpp src/automation/automation-store.cpp src/memory/memory-store.cpp src/memory/memory-search.cpp src/memory/memory-schema.cpp src/swarm/mailbox.cpp src/swarm/team-manager.cpp tests/storage/sqlite-test.cpp tests/storage/session-store-test.cpp tests/automation/automation-store-test.cpp tests/memory/memory-test.cpp tests/swarm/mailbox-test.cpp tests/swarm/team-manager-test.cpp
git commit -m "refactor(storage): complete sqlite wrapper modernization"
```
