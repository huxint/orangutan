#include "memory/memory-store.hpp"
#include "memory/memory-extract.hpp"
#include "memory/runtime-memory.hpp"
#include "app/runtime/memory-context.hpp"
#include "tools/registry/tool.hpp"
#include "infra/utf8.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <set>
#include <sstream>
#include <string>
#include <utility>

using namespace orangutan;

namespace {

    class MemoryStoreHarness {
    public:
        MemoryStoreHarness()
        : db_path_(orangutan::testing::unique_test_db_path("memory-store", "memory.db")) {}

        ~MemoryStoreHarness() {
            std::filesystem::remove_all(db_path_.parent_path());
        }

        [[nodiscard]]
        const std::filesystem::path &db_path() const {
            return db_path_;
        }

    private:
        std::filesystem::path db_path_;
    };

    std::string read_text_file(const std::filesystem::path &path) {
        std::ifstream input(path);
        std::stringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    template <typename Fn>
    bool completes_without_throw(Fn &&fn) {
        try {
            std::forward<Fn>(fn)();
            return true;
        } catch (...) {
            return false;
        }
    }

    TEST_CASE("remember_and_recall_by_key_or_content") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile");
        store.remember("project_context", "Working on orangutan", "project");

        const auto by_key = store.recall("user_name");
        CHECK(by_key.contains("Alice"));

        const auto by_content = store.recall("orangutan");
        CHECK(by_content.contains("project_context"));
        CHECK(by_content.contains("Working on orangutan"));
    };

    TEST_CASE("recall_by_category_returns_only_matching_entries") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("preferred_language", "Python", "preferences");
        store.remember("preferred_editor", "Neovim", "preferences");
        store.remember("project_context", "orangutan", "project");

        const auto entries = store.recall_by_category("preferences");
        REQUIRE(entries.size() == 2UL);

        std::set<std::pair<std::string, std::string>> actual(entries.begin(), entries.end());
        CHECK(actual.contains({"preferred_language", "Python"}));
        CHECK(actual.contains({"preferred_editor", "Neovim"}));
        CHECK_FALSE(actual.contains({"project_context", "orangutan"}));
    };

    TEST_CASE("search_returns_most_relevant_entries_first") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("project.current", "orangutan memory refactor", "project");
        store.remember("misc.note", "buy groceries later", "general");

        const auto results = store.search("project.current");
        REQUIRE(not results.empty());
        CHECK(results.front().key == "project.current");
    };

    TEST_CASE("search_backfills_past_category_cap_when_only_one_category_matches") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("project.alpha", "shared project memory alpha", "project");
        store.remember("project.beta", "shared project memory beta", "project");
        store.remember("project.gamma", "shared project memory gamma", "project");
        store.remember("project.delta", "shared project memory delta", "project");

        const auto results = store.search("shared project memory", {}, 4);
        REQUIRE(results.size() == 4UL);

        std::set<std::string> keys;
        for (const auto &record : results) {
            CHECK(record.category == "project");
            keys.insert(record.key);
        }

        CHECK(keys.contains("project.alpha"));
        CHECK(keys.contains("project.beta"));
        CHECK(keys.contains("project.gamma"));
        CHECK(keys.contains("project.delta"));
    };

    TEST_CASE("forget_removes_existing_entry") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile");

        CHECK(store.forget("user_name"));
        CHECK_FALSE(store.forget("user_name"));
        CHECK(store.recall_by_category("profile").empty());
    };

    TEST_CASE("remember_updates_existing_key") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile");
        store.remember("user_name", "Bob", "profile");

        const auto recall = store.recall("user_name");
        CHECK(recall.contains("Bob"));
        CHECK_FALSE(recall.contains("Alice"));
    };

    TEST_CASE("update_can_merge_distinct_content_fragments") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("preference.general", "concise answers", "preference");
        store.update("preference.general", "short commit messages", "preference");

        const auto recall = store.recall("preference.general");
        CHECK(recall.contains("concise answers"));
        CHECK(recall.contains("short commit messages"));
    };

    TEST_CASE("update_preserves_existing_category_and_source_when_omitted") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("profile.name", "Alice", "profile", {}, "auto:session", 0.9);

        store.update("profile.name", "Alice Example");

        const auto entries = store.list({}, "profile", 10);
        REQUIRE(entries.size() == 1UL);
        CHECK(entries[0].content == "Alice Example");
        CHECK(entries[0].category == "profile");
        CHECK(entries[0].source == "auto:session");

        const auto stats = store.stats();
        CHECK(stats.auto_entries == 1);
        CHECK(stats.manual_entries == 0);
    };

    TEST_CASE("dump_all_includes_stored_memories") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile");
        store.remember("project_context", "Working on orangutan", "project");

        const auto dump = store.dump_all();
        CHECK(dump.contains("user_name"));
        CHECK(dump.contains("Alice"));
        CHECK(dump.contains("project_context"));
        CHECK(dump.contains("Working on orangutan"));
    };

    TEST_CASE("stats_count_manual_and_auto_entries") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile", {}, "manual", 0.9);
        static_cast<void>(store.auto_capture("my name is Bob and we are working on orangutan"));

        const auto stats = store.stats();
        CHECK(stats.total == 3);
        CHECK(stats.categories >= 2);
        CHECK(stats.manual_entries == 1);
        CHECK(stats.auto_entries == 2);
    };

    TEST_CASE("auto_capture_extracts_structured_memories") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("my name is Alice. We are working on orangutan. I prefer concise answers."));

        const auto name = store.recall("profile.name");
        const auto project = store.recall("project.current");
        const auto preference = store.recall("preference.general");

        CHECK(name.contains("Alice"));
        CHECK(project.contains("orangutan"));
        CHECK(preference.contains("concise answers"));
    };

    TEST_CASE("auto_capture_merges_repeated_general_preferences") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("I prefer concise answers."));
        static_cast<void>(store.auto_capture("I prefer short commit messages."));

        const auto preference = store.recall("preference.general");
        CHECK(preference.contains("concise answers"));
        CHECK(preference.contains("short commit messages"));
    };

    TEST_CASE("scoped_memories_do_not_leak_across_identities") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("preferred_name", "Alice", "profile", "jid:alice");
        store.remember("preferred_name", "Bob", "profile", "jid:bob");

        const auto alice = store.recall("preferred_name", "jid:alice");
        const auto bob = store.recall("preferred_name", "jid:bob");
        const auto global = store.dump_all();

        CHECK(alice.contains("Alice"));
        CHECK_FALSE(alice.contains("Bob"));
        CHECK(bob.contains("Bob"));
        CHECK_FALSE(bob.contains("Alice"));
        CHECK(global.empty());
    };

    TEST_CASE("builtin_memory_tools_register_and_execute") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        ToolRegistry registry;
        RuntimeMemory runtime_memory(store);
        register_builtin_tools(registry, &runtime_memory);

        const auto definitions = registry.definitions();
        std::set<std::string> tool_names;
        for (const auto &definition : definitions) {
            tool_names.insert(definition.name);
        }

        CHECK(tool_names.contains("remember"));
        CHECK(tool_names.contains("recall"));
        CHECK(tool_names.contains("forget"));
        CHECK(tool_names.contains("memory_store"));
        CHECK(tool_names.contains("memory_recall"));
        CHECK(tool_names.contains("memory_forget"));
        CHECK(tool_names.contains("memory_update"));
        CHECK(tool_names.contains("memory_list"));
        CHECK(tool_names.contains("memory_stats"));

        auto remember_result = registry.execute(ToolUse("remember-1", "remember", {{"key", "preferred_language"}, {"content", "C++"}, {"category", "preferences"}}));
        CHECK_FALSE(remember_result.is_error);

        auto recall_result = registry.execute(ToolUse("recall-1", "recall", {{"query", "preferred_language"}}));
        CHECK_FALSE(recall_result.is_error);
        CHECK(recall_result.content.contains("C++"));

        auto forget_result = registry.execute(ToolUse("forget-1", "forget", {{"key", "preferred_language"}}));
        CHECK_FALSE(forget_result.is_error);
        CHECK(forget_result.content.contains("Forgot"));
    };

    TEST_CASE("plugin_style_memory_aliases_work") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        ToolRegistry registry;
        RuntimeMemory runtime_memory(store);
        register_builtin_tools(registry, &runtime_memory);

        auto store_result = registry.execute(ToolUse("memory-store-1", "memory_store", {{"key", "profile.name"}, {"content", "Alice"}, {"category", "profile"}}));
        auto recall_result = registry.execute(ToolUse("memory-recall-1", "memory_recall", {{"query", "profile.name"}}));
        auto update_result =
            registry.execute(ToolUse("memory-update-1", "memory_update", {{"key", "profile.name"}, {"content", "Alice Example"}, {"category", "profile"}, {"merge", false}}));
        auto stats_result = registry.execute(ToolUse("memory-stats-1", "memory_stats", nlohmann::json::object()));
        auto list_result = registry.execute(ToolUse("memory-list-1", "memory_list", {{"category", "profile"}}));

        CHECK_FALSE(store_result.is_error);
        CHECK_FALSE(recall_result.is_error);
        CHECK_FALSE(update_result.is_error);
        CHECK_FALSE(stats_result.is_error);
        CHECK_FALSE(list_result.is_error);
        CHECK(recall_result.content.contains("Alice"));
        CHECK(stats_result.content.contains("total=1"));
        CHECK(list_result.content.contains("Alice Example"));
    };

    TEST_CASE("builtin_memory_tools_respect_memory_scope") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        ToolRegistry alice_registry;
        ToolRegistry bob_registry;
        RuntimeMemory alice_memory(store, RuntimeMemoryContext{.scope = "jid:alice"});
        RuntimeMemory bob_memory(store, RuntimeMemoryContext{.scope = "jid:bob"});
        register_builtin_tools(alice_registry, &alice_memory);
        register_builtin_tools(bob_registry, &bob_memory);

        auto alice_remember = alice_registry.execute(ToolUse("remember-alice", "remember", {{"key", "favorite_color"}, {"content", "green"}}));
        auto bob_recall = bob_registry.execute(ToolUse("recall-bob", "recall", {{"query", "favorite_color"}}));
        auto alice_recall = alice_registry.execute(ToolUse("recall-alice", "recall", {{"query", "favorite_color"}}));

        CHECK_FALSE(alice_remember.is_error);
        CHECK_FALSE(bob_recall.is_error);
        CHECK_FALSE(alice_recall.is_error);
        CHECK(bob_recall.content == "(no memories found)");
        CHECK(alice_recall.content.contains("green"));
    };

    TEST_CASE("migrates_legacy_memory_table_to_structured_schema") {
        MemoryStoreHarness harness;
        sqlite3 *db = nullptr;
        const auto open_status = sqlite3_open(harness.db_path().string().c_str(), &db);
        REQUIRE(open_status == SQLITE_OK);

        char *err_msg = nullptr;
        const auto create_status = sqlite3_exec(db,
                                                R"(
                                                CREATE TABLE memories (
                                                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                                                    key TEXT NOT NULL UNIQUE,
                                                    content TEXT NOT NULL,
                                                    category TEXT NOT NULL DEFAULT 'general',
                                                    created_at TEXT NOT NULL DEFAULT (datetime('now')),
                                                    updated_at TEXT NOT NULL DEFAULT (datetime('now'))
                                                );
                                            )",
                                                nullptr, nullptr, &err_msg);
        INFO((err_msg != nullptr ? err_msg : "sqlite error"));
        REQUIRE(create_status == SQLITE_OK);
        sqlite3_free(err_msg);
        err_msg = nullptr;

        const auto insert_status =
            sqlite3_exec(db, "INSERT INTO memories (key, content, category) VALUES ('s:jid:alice\x1fpreferred_name', 'Alice', 'profile');", nullptr, nullptr, &err_msg);
        INFO((err_msg != nullptr ? err_msg : "sqlite error"));
        REQUIRE(insert_status == SQLITE_OK);
        sqlite3_free(err_msg);
        sqlite3_close(db);

        MemoryStore store(harness.db_path());
        const auto recall = store.recall("preferred_name", "jid:alice");
        CHECK(recall.contains("Alice"));
    };

    TEST_CASE("auto_capture_chinese_name_produces_valid_utf8") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("我是一个算法高手"));

        const auto name = store.recall("profile.name");
        CHECK(completes_without_throw([&] {
            nlohmann::json j = name;
            static_cast<void>(j.dump());
        }));
        if (!name.contains("(no memories found)")) {
            CHECK(name.contains("\xe4\xb8\x80"));
        }
    };

    TEST_CASE("auto_capture_chinese_remember_produces_valid_utf8") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("记住我是一个算法高手"));

        const auto stats = store.stats();
        CHECK(stats.auto_entries >= 1);

        const auto dump = store.dump_all();
        CHECK(completes_without_throw([&] {
            nlohmann::json j = dump;
            static_cast<void>(j.dump());
        }));
    };

    TEST_CASE("auto_capture_accented_latin_produces_valid_utf8") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("my name is Jos\xc3\xa9"));

        const auto name = store.recall("profile.name");
        CHECK(completes_without_throw([&] {
            nlohmann::json j = name;
            static_cast<void>(j.dump());
        }));
        CHECK(name.contains("Jos\xc3\xa9"));
    };

    TEST_CASE("auto_capture_emoji_produces_valid_utf8") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("remember that the deploy emoji is \xf0\x9f\x9a\x80"));

        const auto stats = store.stats();
        CHECK(stats.auto_entries >= 1);

        const auto dump = store.dump_all();
        CHECK(completes_without_throw([&] {
            nlohmann::json j = dump;
            static_cast<void>(j.dump());
        }));
    };

    TEST_CASE("stats_expose_journal_entries_separately") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("project.current", "orangutan memory refactor", "project", {}, "session:distilled", 0.9);
        store.remember("journal.1", "Reviewed recall ranking and mirror design", "journal", {}, "session:journal", 0.4);

        const auto stats = store.stats();
        CHECK(stats.total == 2);
        CHECK(stats.journal_entries == 1);
    };

    TEST_CASE("runtime_memory_refreshes_managed_snapshot_for_durable_memories") {
        const auto workspace = orangutan::testing::unique_test_root("memory-snapshot");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });

        runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);

        const auto snapshot = workspace / "MEMORY.md";
        REQUIRE(std::filesystem::exists(snapshot));
        const auto text = read_text_file(snapshot);
        CHECK(text.contains("<!-- ORANGUTAN:MEMORY:BEGIN -->"));
        CHECK(text.contains("orangutan memory enhancements"));
        CHECK_FALSE(text.contains("session:journal"));

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("runtime_memory_disabled_mirror_does_not_create_snapshot") {
        const auto workspace = orangutan::testing::unique_test_root("memory-disabled-mirror");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = false, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });

        runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);
        CHECK_FALSE(std::filesystem::exists(workspace / "MEMORY.md"));

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("runtime_memory_skips_malformed_managed_section") {
        const auto workspace = orangutan::testing::unique_test_root("memory-invalid-layout");
        const auto snapshot = workspace / "MEMORY.md";

        {
            std::ofstream out(snapshot);
            out << "User notes\n<!-- ORANGUTAN:MEMORY:BEGIN -->\nmissing end marker\n";
        }

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });
        runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);

        const auto refresh = runtime.refresh_mirror();
        CHECK(refresh.skipped);
        CHECK(read_text_file(snapshot) == "User notes\n<!-- ORANGUTAN:MEMORY:BEGIN -->\nmissing end marker\n");

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("runtime_memory_skips_refresh_when_managed_markers_are_duplicated") {
        const auto workspace = orangutan::testing::unique_test_root("memory-duplicate-marker");
        const auto snapshot = workspace / "MEMORY.md";

        const std::string original = "User notes\n"
                                     "<!-- ORANGUTAN:MEMORY:BEGIN -->\n"
                                     "old generated block\n"
                                     "<!-- ORANGUTAN:MEMORY:END -->\n"
                                     "stray marker <!-- ORANGUTAN:MEMORY:BEGIN -->\n";
        {
            std::ofstream out(snapshot);
            out << original;
        }

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });
        runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);

        const auto refresh = runtime.refresh_mirror();
        CHECK(refresh.skipped);
        CHECK(read_text_file(snapshot) == original);

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("runtime_memory_stores_journal_summary_and_appends_daily_journal_file") {
        const auto workspace = orangutan::testing::unique_test_root("memory-journal");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });

        const auto result = runtime.store_journal_summary("Reviewed ranking and mirror behavior.");
        CHECK(result.stored);
        CHECK(result.mirrored);

        const auto journal_entries = runtime.list("journal", 10);
        REQUIRE(journal_entries.size() == 1UL);
        CHECK(journal_entries[0].source == "session:journal");
        CHECK(journal_entries[0].content.contains("Reviewed ranking and mirror behavior."));

        const auto journal_root = workspace / "memory";
        REQUIRE(std::filesystem::exists(journal_root));
        auto it = std::filesystem::directory_iterator(journal_root);
        REQUIRE(it != std::filesystem::directory_iterator{});
        const auto journal_text = read_text_file(it->path());
        CHECK(journal_text.contains("Reviewed ranking and mirror behavior."));

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("search_falls_back_for_non_ascii_queries") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("fact.algorithms", "算法高手", "profile");

        const auto results = store.search("算法", {}, 4);
        REQUIRE(not results.empty());
        CHECK(results.front().key == "fact.algorithms");
    };

    TEST_CASE("runtime_memory_mirror_refresh_is_scope_aware") {
        const auto workspace = orangutan::testing::unique_test_root("memory-scope-aware");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory alpha(store, RuntimeMemoryContext{
                                       .scope = "scope:alpha",
                                       .workspace = workspace.string(),
                                       .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                   });
        RuntimeMemory beta(store, RuntimeMemoryContext{
                                      .scope = "scope:beta",
                                      .workspace = workspace.string(),
                                      .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                  });

        alpha.remember("project.current", "alpha memory", "project", "session:distilled", 0.9);
        beta.remember("project.current", "beta memory", "project", "session:distilled", 0.9);

        const auto alpha_refresh = alpha.refresh_mirror();
        CHECK(alpha_refresh.refreshed);
        auto snapshot = read_text_file(workspace / "MEMORY.md");
        CHECK(snapshot.contains("alpha memory"));
        CHECK_FALSE(snapshot.contains("beta memory"));

        const auto beta_refresh = beta.refresh_mirror();
        CHECK(beta_refresh.refreshed);
        snapshot = read_text_file(workspace / "MEMORY.md");
        CHECK(snapshot.contains("beta memory"));
        CHECK_FALSE(snapshot.contains("alpha memory"));

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("chinese_name_stops_at_cjk_comma") {
        const auto candidates = memory_detail::extract_auto_candidates("我叫阿明，做编译器");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.key == "profile.name" && candidate.content == "阿明") {
                found = true;
                break;
            }
        }
        CHECK(found);
    };

    TEST_CASE("chinese_remember_skips_fullwidth_colon") {
        const auto candidates = memory_detail::extract_auto_candidates("请记住：算法高手。");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.category == "fact" && candidate.content == "算法高手") {
                found = true;
                break;
            }
        }
        CHECK(found);
    };

    TEST_CASE("chinese_project_matches_mid_utterance_with_fixed_key") {
        const auto candidates = memory_detail::extract_auto_candidates("好的，我们在做编译器。");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.key == "project.current" && candidate.category == "project" && candidate.content == "编译器") {
                found = true;
                break;
            }
        }
        CHECK(found);
    };

    TEST_CASE("chinese_remember_matches_mid_utterance_with_hashed_key") {
        const auto candidates = memory_detail::extract_auto_candidates("好的，请记住：算法高手。");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.category == "fact" && candidate.content == "算法高手" && candidate.key.rfind("fact.note.", 0) == 0) {
                found = true;
                break;
            }
        }
        CHECK(found);
    };

    TEST_CASE("chinese_name_stops_at_invalid_utf8") {
        const auto candidates = memory_detail::extract_auto_candidates(std::string{"我叫阿"} + "\xff" + "明");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.key == "profile.name" && candidate.content == "阿") {
                found = true;
                break;
            }
        }
        CHECK(found);
    };

    TEST_CASE("ascii_passes_through") {
        CHECK(utf8::sanitize("hello world") == std::string{"hello world"});
        CHECK(utf8::sanitize("") == std::string{""});
    };

    TEST_CASE("valid_chinese_passes_through") {
        const std::string input = "\xe7\xae\x97\xe6\xb3\x95\xe9\xab\x98\xe6\x89\x8b";
        CHECK(utf8::sanitize(input) == input);
    };

    TEST_CASE("truncated_chinese_character_is_stripped") {
        const std::string truncated = "\xe7\xae";
        CHECK(utf8::sanitize(truncated) == std::string{""});
    };

    TEST_CASE("valid_text_followed_by_truncated_character") {
        const std::string input = std::string{"hi"} + "\xe4\xb8";
        CHECK(utf8::sanitize(input) == std::string{"hi"});
    };

    TEST_CASE("valid_chinese_followed_by_truncated_character") {
        const std::string input = "\xe7\xae\x97\xe6";
        CHECK(utf8::sanitize(input) == std::string{"\xe7\xae\x97"});
    };

    TEST_CASE("accented_latin_passes_through") {
        const std::string input = "Jos\xc3\xa9";
        CHECK(utf8::sanitize(input) == input);
    };

    TEST_CASE("truncated_accented_latin_is_stripped") {
        const std::string input = "Jos\xc3";
        CHECK(utf8::sanitize(input) == std::string{"Jos"});
    };

    TEST_CASE("emoji_passes_through") {
        const std::string input = "\xf0\x9f\x9a\x80";
        CHECK(utf8::sanitize(input) == input);
    };

    TEST_CASE("truncated_emoji_is_stripped") {
        const std::string input = "go\xf0\x9f\x9a";
        CHECK(utf8::sanitize(input) == std::string{"go"});
    };

    TEST_CASE("invalid_leading_byte_is_skipped_and_later_text_survives") {
        const std::string input = std::string{"ok"} + "\xff" + "再见";
        CHECK(utf8::sanitize(input) == std::string{"ok再见"});
    };

    TEST_CASE("stray_continuation_bytes_are_skipped") {
        const std::string input = std::string{"ok"} + "\x80\xbf" + "fine";
        CHECK(utf8::sanitize(input) == std::string{"okfine"});
    };

} // namespace
