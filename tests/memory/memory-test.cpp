#include "memory/memory-store.hpp"
#include "memory/runtime-memory.hpp"
#include "tools/registry/tool-registry.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <set>
#include <string_view>

using namespace orangutan;

namespace {

    class MemoryStoreHarness {
    public:
        MemoryStoreHarness()
        : db_path_(orangutan::testing::unique_test_db_path("memory-store", "memory.db")) {}

        ~MemoryStoreHarness() {
            std::filesystem::remove_all(db_path_.parent_path());
        }
        MemoryStoreHarness(const MemoryStoreHarness &) = delete;
        MemoryStoreHarness &operator=(const MemoryStoreHarness &) = delete;
        MemoryStoreHarness(MemoryStoreHarness &&) = delete;
        MemoryStoreHarness &operator=(MemoryStoreHarness &&) = delete;

        [[nodiscard]]
        const std::filesystem::path &db_path() const {
            return db_path_;
        }

    private:
        std::filesystem::path db_path_;
    };

    const ToolDef *find_tool(const std::vector<ToolDef> &definitions, std::string_view name) {
        const auto it = std::ranges::find_if(definitions, [name](const ToolDef &definition) {
            return definition.name == name;
        });
        return it == definitions.end() ? nullptr : &*it;
    }

    std::set<std::string> discover_deferred_tool_names(ToolRegistry &registry) {
        std::set<std::string> names;
        for (const auto &summary : registry.deferred_tool_summaries()) {
            names.insert(std::string(summary.name));
            registry.discover_tool(std::string(summary.name));
        }
        return names;
    }

    TEST_CASE("memory_store_remembers_recalls_and_forgets_by_scope") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());

        store.remember("preference.reply-style", "The user prefers concise direct replies.", memory_type::user, "scope:alice");
        store.remember("preference.reply-style", "The user prefers detailed replies.", memory_type::user, "scope:bob");

        const auto alice = store.recall("reply style", "scope:alice");
        const auto bob = store.recall("reply style", "scope:bob");

        CHECK(alice.contains("concise direct replies"));
        CHECK_FALSE(alice.contains("detailed replies"));
        CHECK(bob.contains("detailed replies"));
        CHECK_FALSE(bob.contains("concise direct replies"));

        CHECK(store.forget("preference.reply-style", "scope:alice"));
        CHECK_FALSE(store.recall("reply style", "scope:alice").contains("concise direct replies"));
    }

    TEST_CASE("memory_store_replaces_existing_key_without_merge_semantics") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());

        store.remember("project.current", "Working on the old memory implementation.", memory_type::project, "scope:test");
        store.remember("project.current", "Working on the new memory implementation.", memory_type::project, "scope:test");

        const auto recall = store.recall("project.current", "scope:test");
        CHECK(recall.contains("new memory implementation"));
        CHECK_FALSE(recall.contains("old memory implementation"));
    }

    TEST_CASE("memory_search_ranks_key_matches_before_content_matches") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());

        store.remember("project.current", "The memory refactor is underway.", memory_type::project, "scope:test");
        store.remember("note.project", "project.current appears in this note only as text.", memory_type::reference, "scope:test");

        const auto records = store.search("project.current", "scope:test");
        REQUIRE_FALSE(records.empty());
        CHECK(records.front().key == "project.current");
        CHECK(records.front().kind == memory_type::project);
    }

    TEST_CASE("memory_search_recovers_key_matches_outside_recent_scan_window") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());

        store.remember("profile.legacy", "Keep this long-lived memory reachable.", memory_type::user, "scope:test");
        for (int index = 0; index < 260; ++index) {
            store.remember("recent.note." + std::to_string(index), "Recent filler memory " + std::to_string(index), memory_type::project, "scope:test");
        }

        const auto records = store.search("profile.legacy", "scope:test");

        REQUIRE_FALSE(records.empty());
        CHECK(std::ranges::any_of(records, [](const MemoryRecord &record) {
            return record.key == "profile.legacy";
        }));
    }

    TEST_CASE("runtime_memory_binds_store_operations_to_current_scope") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory alice(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "scope:alice"});
        RuntimeMemory bob(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "scope:bob"});

        alice.remember("user.name", "Alice", memory_type::user);

        CHECK(alice.recall("user.name").contains("Alice"));
        CHECK_FALSE(bob.recall("user.name").contains("Alice"));
    }

    TEST_CASE("memory_tools_register_only_current_triplet") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory memory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "scope:test"});
        ToolRegistry registry;
        register_builtin_tools(registry, &memory);

        const auto deferred_names = discover_deferred_tool_names(registry);

        CHECK(deferred_names.contains("forget"));
        CHECK(deferred_names.contains("recall"));
        CHECK(deferred_names.contains("remember"));

        const auto definitions = registry.definitions();
        CHECK(find_tool(definitions, "remember") != nullptr);
        CHECK(find_tool(definitions, "recall") != nullptr);
        CHECK(find_tool(definitions, "forget") != nullptr);
    }

    TEST_CASE("memory_tools_execute_current_triplet") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory memory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "scope:test"});
        ToolRegistry registry;
        register_builtin_tools(registry, &memory);

        auto remember = registry.execute(ToolUse("remember-1", "remember", { {"key", "preference.reply-style"}, {"content", "Be concise."}, {"kind", "feedback"} }));
        auto recall = registry.execute(ToolUse("recall-1", "recall", { {"query", "reply style"} }));
        auto forget = registry.execute(ToolUse("forget-1", "forget", { {"key", "preference.reply-style"} }));
        auto recall_after_forget = registry.execute(ToolUse("recall-2", "recall", { {"query", "reply style"} }));

        CHECK_FALSE(remember.is_error);
        CHECK_FALSE(recall.is_error);
        CHECK_FALSE(forget.is_error);
        CHECK_FALSE(recall_after_forget.is_error);
        CHECK(remember.content.contains("feedback"));
        CHECK(recall.content.contains("Be concise."));
        CHECK(forget.content.contains("Forgot"));
        CHECK(recall_after_forget.content == "(no memories found)");
    }

    TEST_CASE("memory_tools_reject_invalid_kind") {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory memory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "scope:test"});
        ToolRegistry registry;
        register_builtin_tools(registry, &memory);

        const auto result = registry.execute(ToolUse("remember-invalid", "remember", { {"key", "preference.reply-style"}, {"content", "Be concise."}, {"kind", "note"} }));

        CHECK(result.is_error);
        CHECK(result.content.contains("memory kind must be one of"));
        CHECK(memory.recall("reply style") == "");
    }

    TEST_CASE("memory_store_keeps_new_schema_data_across_reopen") {
        const auto db_path = orangutan::testing::unique_test_db_path("memory-store", "persist.db");
        {
            MemoryStore store(db_path);
            store.remember("project.current", "new value", memory_type::project, "scope:test");
        }
        {
            MemoryStore store(db_path);
            CHECK(store.recall("project.current", "scope:test").contains("new value"));
        }

        std::filesystem::remove_all(db_path.parent_path());
    }

} // namespace
