#include "memory/memory-store.hpp"
#include "memory/runtime-memory.hpp"
#include "memory/memory-age.hpp"
#include "bootstrap/memory-context.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/utf8.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
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

    const ToolDef *find_tool(const std::vector<ToolDef> &definitions, std::string_view name) {
        const auto it = std::ranges::find_if(definitions, [name](const ToolDef &definition) {
            return definition.name == name;
        });
        return it == definitions.end() ? nullptr : &*it;
    }

    class BuiltinMemoryToolsHarness {
    public:
        BuiltinMemoryToolsHarness()
        : store_(store_harness_.db_path()),
          runtime_memory_(store_) {
            register_builtin_tools(registry, &runtime_memory_);
        }

    private:
        MemoryStoreHarness store_harness_;

    public:
        MemoryStore store_;
        ToolRegistry registry;
        RuntimeMemory runtime_memory_;
    };

    std::set<std::string> discover_deferred_tool_names(ToolRegistry &registry) {
        std::set<std::string> names;
        for (const auto &summary : registry.deferred_tool_summaries()) {
            names.insert(std::string(summary.name));
            registry.discover_tool(std::string(summary.name));
        }
        return names;
    }

    std::vector<ToolDef> discover_tool_definitions(ToolRegistry &registry) {
        registry.discover_deferred_tools();
        return registry.definitions();
    }

    nlohmann::json remember_input_schema() {
        return nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"key", {{"type", "string"}, {"description", "Stable lookup key for the memory"}}},
              {"content", {{"type", "string"}, {"description", "The memory value to store"}}},
              {"category", {{"type", "string"}, {"description", "Granular category label (e.g. profile, preference, project)"}}},
              {"type", {{"type", "string"}, {"enum", nlohmann::json::array({"user", "feedback", "project", "reference"})}, {"description", "Semantic memory type"}}},
              {"source", {{"type", "string"}, {"description", "Optional memory source label"}}},
              {"importance", {{"type", "number"}, {"description", "Optional importance score from 0 to 1"}}}}},
            {"required", nlohmann::json::array({"key", "content"})}};
    }

    nlohmann::json memory_store_input_schema() {
        return nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"key", {{"type", "string"}, {"description", "Stable lookup key for the memory"}}},
              {"content", {{"type", "string"}, {"description", "The memory value to store"}}},
              {"category", {{"type", "string"}, {"description", "Granular category label"}}},
              {"type", {{"type", "string"}, {"enum", nlohmann::json::array({"user", "feedback", "project", "reference"})}, {"description", "Semantic memory type"}}},
              {"source", {{"type", "string"}, {"description", "Optional memory source label"}}},
              {"importance", {{"type", "number"}, {"description", "Optional importance score from 0 to 1"}}}}},
            {"required", nlohmann::json::array({"key", "content"})}};
    }

    nlohmann::json recall_input_schema() {
        return nlohmann::json{{"type", "object"},
                              {"additionalProperties", false},
                              {"properties",
                               {{"mode",
                                 {{"type", "string"},
                                  {"enum", nlohmann::json::array({"query", "category"})},
                                  {"description", "How to recall memories: 'query' searches keys/content, 'category' lists a category"}}},
                                {"value", {{"type", "string"}, {"description", "Search text or category name, depending on mode"}}},
                                {"limit", {{"type", "integer"}, {"description", "Maximum number of memories to return"}}}}},
                              {"required", nlohmann::json::array({"mode", "value"})}};
    }

    nlohmann::json forget_input_schema() {
        return nlohmann::json{
            {"type", "object"}, {"properties", {{"key", {{"type", "string"}, {"description", "The memory key to delete"}}}}}, {"required", nlohmann::json::array({"key"})}};
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

    TEST_CASE("memory_age_invalid_timestamp_reports_unknown") {
        CHECK(memory_age_days("invalid") < 0);
        CHECK(memory_age_text("invalid") == "unknown age");
        CHECK(memory_freshness_caveat("invalid").empty());
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

    TEST_CASE("search_excludes_memories_without_any_query_match") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("project.current", "orangutan memory refactor", "project", memory_type::user, {}, "manual", 0.9);
        store.remember("misc.note", "buy groceries later", "general", memory_type::user, {}, "manual", 1.0);

        const auto results = store.search("quantum banana");

        CHECK(results.empty());
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
        store.remember("profile.name", "Alice", "profile", memory_type::user, {}, "auto:session", 0.9);

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

    TEST_CASE("stats_count_manual_entries") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile", memory_type::user, {}, "manual", 0.9);
        store.remember("project_context", "orangutan", "project");

        const auto stats = store.stats();
        CHECK(stats.total == 2);
        CHECK(stats.categories >= 2);
        CHECK(stats.manual_entries == 2);
    };

    TEST_CASE("scoped_memories_do_not_leak_across_identities") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("preferred_name", "Alice", "profile", memory_type::user, "jid:alice");
        store.remember("preferred_name", "Bob", "profile", memory_type::user, "jid:bob");

        const auto alice = store.recall("preferred_name", "jid:alice");
        const auto bob = store.recall("preferred_name", "jid:bob");
        const auto global = store.dump_all();

        CHECK(alice.contains("Alice"));
        CHECK_FALSE(alice.contains("Bob"));
        CHECK(bob.contains("Bob"));
        CHECK_FALSE(bob.contains("Alice"));
        CHECK(global.empty());
    };
    TEST_CASE("builtin_memory_tools_advertise_expected_deferred_names") {
        BuiltinMemoryToolsHarness harness;

        // Memory tools are registered as deferred — not visible in definitions() until discovered.
        // Verify they exist by discovering them and then checking definitions.
        CHECK(harness.registry.has_deferred_tools());
        const auto deferred_names = discover_deferred_tool_names(harness.registry);

        CHECK(deferred_names.contains("remember"));
        CHECK(deferred_names.contains("recall"));
        CHECK(deferred_names.contains("forget"));
        CHECK(deferred_names.contains("memory_store"));
        CHECK(deferred_names.contains("memory_recall"));
        CHECK(deferred_names.contains("memory_forget"));
        CHECK(deferred_names.contains("memory_update"));
        CHECK(deferred_names.contains("memory_list"));
        CHECK(deferred_names.contains("memory_stats"));
    };

    TEST_CASE("remember_and_memory_store_definitions_match_current_contracts") {
        BuiltinMemoryToolsHarness harness;
        const auto definitions = discover_tool_definitions(harness.registry);
        const auto *remember_definition = find_tool(definitions, "remember");
        const auto *memory_store_definition = find_tool(definitions, "memory_store");

        REQUIRE(remember_definition != nullptr);
        REQUIRE(memory_store_definition != nullptr);

        CHECK(remember_definition->name == "remember");
        CHECK(memory_store_definition->name == "memory_store");

        CHECK(remember_definition->description == "Store a durable fact, preference, or project context for future conversations. Use meaningful, stable keys like 'project.lang', "
                                                  "'preference.style', or 'decision.auth-method'. Type must be one of: user (role/preferences/knowledge), feedback "
                                                  "(corrections/approaches), project (work/decisions/deadlines), reference (external pointers).");
        CHECK(remember_definition->input_schema == remember_input_schema());

        CHECK(memory_store_definition->description == "Plugin-style alias for remember. Type must be one of: user, feedback, project, reference.");
        CHECK(memory_store_definition->input_schema == memory_store_input_schema());
    };

    TEST_CASE("recall_and_memory_recall_definitions_match_current_contracts") {
        BuiltinMemoryToolsHarness harness;
        const auto definitions = discover_tool_definitions(harness.registry);
        const auto *recall_definition = find_tool(definitions, "recall");
        const auto *memory_recall_definition = find_tool(definitions, "memory_recall");

        REQUIRE(recall_definition != nullptr);
        REQUIRE(memory_recall_definition != nullptr);

        CHECK(recall_definition->name == "recall");
        CHECK(memory_recall_definition->name == "memory_recall");
        CHECK(recall_definition->description == "Recall stored memories. Use mode='query' with value=<search text>, or mode='category' with value=<category name>.");
        CHECK(memory_recall_definition->description == "Plugin-style alias for recall. Use mode='query' or mode='category' with value=<text>.");
        CHECK(recall_definition->input_schema == memory_recall_definition->input_schema);
        CHECK(recall_definition->input_schema == recall_input_schema());
        CHECK_FALSE(memory_recall_definition->input_schema.contains("anyOf"));
    };

    TEST_CASE("forget_and_memory_forget_definitions_match_current_contracts") {
        BuiltinMemoryToolsHarness harness;
        const auto definitions = discover_tool_definitions(harness.registry);
        const auto *forget_definition = find_tool(definitions, "forget");
        const auto *memory_forget_definition = find_tool(definitions, "memory_forget");

        REQUIRE(forget_definition != nullptr);
        REQUIRE(memory_forget_definition != nullptr);

        CHECK(forget_definition->name == "forget");
        CHECK(memory_forget_definition->name == "memory_forget");
        CHECK(forget_definition->description == "Delete a stored memory by key.");
        CHECK(memory_forget_definition->description == "Plugin-style alias for forget.");
        CHECK(forget_definition->input_schema == memory_forget_definition->input_schema);
        CHECK(forget_definition->input_schema == forget_input_schema());
    };

    TEST_CASE("builtin_memory_tools_execute_core_triplet") {
        BuiltinMemoryToolsHarness harness;

        auto remember_result = harness.registry.execute(ToolUse("remember-1", "remember", {{"key", "preferred_language"}, {"content", "C++"}, {"category", "preferences"}}));
        CHECK_FALSE(remember_result.is_error);

        auto recall_result = harness.registry.execute(ToolUse("recall-1", "recall", {{"mode", "query"}, {"value", "preferred_language"}}));
        CHECK_FALSE(recall_result.is_error);
        CHECK(recall_result.content.contains("C++"));

        auto forget_result = harness.registry.execute(ToolUse("forget-1", "forget", {{"key", "preferred_language"}}));
        CHECK_FALSE(forget_result.is_error);
        CHECK(forget_result.content.contains("Forgot"));
    };

    TEST_CASE("recall_rejects_missing_mode_or_value") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        ToolRegistry registry;
        RuntimeMemory runtime_memory(store);
        register_builtin_tools(registry, &runtime_memory);

        auto remember_result = registry.execute(ToolUse("remember-1", "remember", {{"key", "profile.name"}, {"content", "Alice"}, {"category", "profile"}}));
        CHECK_FALSE(remember_result.is_error);

        const auto portable_query = registry.execute(ToolUse("recall-query", "recall", {{"mode", "query"}, {"value", "profile.name"}}));
        const auto portable_category = registry.execute(ToolUse("recall-category", "memory_recall", {{"mode", "category"}, {"value", "profile"}}));
        const auto missing_mode = registry.execute(ToolUse("no-mode", "recall", {{"value", "profile.name"}}));
        const auto missing_value = registry.execute(ToolUse("no-value", "recall", {{"mode", "query"}}));
        const auto empty_input = registry.execute(ToolUse("empty", "recall", nlohmann::json::object()));

        CHECK_FALSE(portable_query.is_error);
        CHECK_FALSE(portable_category.is_error);
        CHECK(portable_query.content.contains("Alice"));
        CHECK(portable_category.content.contains("Alice"));
        CHECK(missing_mode.is_error);
        CHECK(missing_value.is_error);
        CHECK(empty_input.is_error);
        CHECK(empty_input.content.contains("recall expects 'mode' (query|category) and 'value'"));
    };

    TEST_CASE("plugin_style_memory_aliases_work") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        ToolRegistry registry;
        RuntimeMemory runtime_memory(store);
        register_builtin_tools(registry, &runtime_memory);

        auto store_result = registry.execute(ToolUse("memory-store-1", "memory_store", {{"key", "profile.name"}, {"content", "Alice"}, {"category", "profile"}}));
        auto recall_result = registry.execute(ToolUse("memory-recall-1", "memory_recall", {{"mode", "query"}, {"value", "profile.name"}}));
        auto update_result =
            registry.execute(ToolUse("memory-update-1", "memory_update", {{"key", "profile.name"}, {"content", "Alice Example"}, {"category", "profile"}, {"merge", false}}));
        auto stats_result = registry.execute(ToolUse("memory-stats-1", "memory_stats", nlohmann::json::object()));
        auto list_result = registry.execute(ToolUse("memory-list-1", "memory_list", {{"category", "profile"}}));
        auto forget_result = registry.execute(ToolUse("memory-forget-1", "memory_forget", {{"key", "profile.name"}}));
        auto recall_after_forget = registry.execute(ToolUse("memory-recall-2", "memory_recall", {{"mode", "query"}, {"value", "profile.name"}}));

        CHECK_FALSE(store_result.is_error);
        CHECK_FALSE(recall_result.is_error);
        CHECK_FALSE(update_result.is_error);
        CHECK_FALSE(stats_result.is_error);
        CHECK_FALSE(list_result.is_error);
        CHECK_FALSE(forget_result.is_error);
        CHECK_FALSE(recall_after_forget.is_error);
        CHECK(recall_result.content.contains("Alice"));
        CHECK(stats_result.content.contains("total=1"));
        CHECK(list_result.content.contains("Alice Example"));
        CHECK(forget_result.content.contains("Forgot"));
        CHECK(recall_after_forget.content == "(no memories found)");
    };

    TEST_CASE("builtin_memory_tools_respect_memory_scope") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        ToolRegistry alice_registry;
        ToolRegistry bob_registry;
        RuntimeMemory alice_memory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "jid:alice"});
        RuntimeMemory bob_memory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "jid:bob"});
        register_builtin_tools(alice_registry, &alice_memory);
        register_builtin_tools(bob_registry, &bob_memory);

        auto alice_remember = alice_registry.execute(ToolUse("remember-alice", "remember", {{"key", "favorite_color"}, {"content", "green"}}));
        auto bob_recall = bob_registry.execute(ToolUse("recall-bob", "recall", {{"mode", "query"}, {"value", "favorite_color"}}));
        auto alice_recall = alice_registry.execute(ToolUse("recall-alice", "recall", {{"mode", "query"}, {"value", "favorite_color"}}));

        CHECK_FALSE(alice_remember.is_error);
        CHECK_FALSE(bob_recall.is_error);
        CHECK_FALSE(alice_recall.is_error);
        CHECK(bob_recall.content == "(no memories found)");
        CHECK(alice_recall.content.contains("green"));
    };

    TEST_CASE("stats_expose_journal_entries_separately") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("project.current", "orangutan memory refactor", "project", memory_type::project, {}, "session:distilled", 0.9);
        store.remember("journal.1", "Reviewed recall ranking and mirror design", "journal", memory_type::project, {}, "session:journal", 0.4);

        const auto stats = store.stats();
        CHECK(stats.total == 2);
        CHECK(stats.journal_entries == 1);
    };

    TEST_CASE("runtime_memory_refreshes_managed_snapshot_for_durable_memories") {
        const auto workspace = orangutan::testing::unique_test_root("memory-snapshot");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, orangutan::bootstrap::RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });

        runtime.remember("project.current", "orangutan memory enhancements", "project", memory_type::project, "session:distilled", 0.9);

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
        RuntimeMemory runtime(store, orangutan::bootstrap::RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = false, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });

        runtime.remember("project.current", "orangutan memory enhancements", "project", memory_type::project, "session:distilled", 0.9);
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
        RuntimeMemory runtime(store, orangutan::bootstrap::RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });
        runtime.remember("project.current", "orangutan memory enhancements", "project", memory_type::project, "session:distilled", 0.9);

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
        RuntimeMemory runtime(store, orangutan::bootstrap::RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });
        runtime.remember("project.current", "orangutan memory enhancements", "project", memory_type::project, "session:distilled", 0.9);

        const auto refresh = runtime.refresh_mirror();
        CHECK(refresh.skipped);
        CHECK(read_text_file(snapshot) == original);

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("runtime_memory_stores_journal_summary_and_appends_daily_journal_file") {
        const auto workspace = orangutan::testing::unique_test_root("memory-journal");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, orangutan::bootstrap::RuntimeMemoryContext{
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
        RuntimeMemory alpha(store, orangutan::bootstrap::RuntimeMemoryContext{
                                       .scope = "scope:alpha",
                                       .workspace = workspace.string(),
                                       .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                   });
        RuntimeMemory beta(store, orangutan::bootstrap::RuntimeMemoryContext{
                                      .scope = "scope:beta",
                                      .workspace = workspace.string(),
                                      .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                  });

        alpha.remember("project.current", "alpha memory", "project", memory_type::project, "session:distilled", 0.9);
        beta.remember("project.current", "beta memory", "project", memory_type::project, "session:distilled", 0.9);

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
