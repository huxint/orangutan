#include "tools/inbox/inbox-tool.hpp"

#include "tools/registry/tool.hpp"
#include "automation/scheduler.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

    struct InboxToolHarness {
        explicit InboxToolHarness(std::string_view filename)
        : db_path(orangutan::testing::unique_test_db_path("inbox-tool", filename)),
          store(db_path.string()),
          runtime(store),
          context{.agent_key = "default", .automation_runtime = &runtime} {
            orangutan::tools::register_inbox_tool(registry, &context);
        }

        std::filesystem::path db_path;
        orangutan::automation::Store store;
        orangutan::automation::Runtime runtime;
        orangutan::ToolRuntimeContext context;
        orangutan::ToolRegistry registry;
    };

    TEST_CASE("inbox_unknown_op_returns_exact_error") {
        InboxToolHarness harness("inbox-tool-unknown-op.db");

        const auto result = harness.registry.execute(orangutan::ToolUse("inbox-unknown-op", "inbox", {{"op", "noop"}, {"id", "item-1"}}));

        CHECK(result.content == "Error: unknown operation. Supported: list, ack, clear.");
    }

    TEST_CASE("inbox_ack_requires_id_with_exact_error") {
        InboxToolHarness harness("inbox-tool-missing-id.db");

        const auto result = harness.registry.execute(orangutan::ToolUse("inbox-ack-missing-id", "inbox", {{"op", "ack"}}));

        CHECK(result.content == "Error: id is required.");
    }

    TEST_CASE("inbox_registered_tool_reports_unavailable_context_after_runtime_is_removed") {
        InboxToolHarness harness("inbox-tool-runtime-removed.db");
        REQUIRE(harness.registry.find_definition("inbox") != nullptr);

        harness.context.automation_runtime = nullptr;

        const auto result = harness.registry.execute(orangutan::ToolUse("inbox-list-no-runtime", "inbox", {{"op", "list"}}));

        CHECK(result.content == "Error: inbox tool is not available in this context.");
    }

    TEST_CASE("inbox_tool_definition_matches_exact_schema") {
        InboxToolHarness harness("inbox-tool-definition.db");

        const auto *definition = harness.registry.find_definition("inbox");
        REQUIRE(definition != nullptr);
        CHECK(definition->name == "inbox");
        CHECK(definition->description == "Inspect and manage unread automation delivery results for the current agent.");
        CHECK(definition->input_schema == nlohmann::json{
                                              {"type", "object"},
                                              {"properties",
                                               {
                                                   {"op", {{"type", "string"}, {"enum", nlohmann::json::array({"list", "ack", "clear"})}}},
                                                   {"id", {{"type", "string"}}},
                                               }},
                                              {"required", nlohmann::json::array({"op"})},
                                          });
    }

} // namespace

TEST_CASE("registers_when_automation_runtime_is_present") {
    InboxToolHarness harness("inbox-tool-register.db");

    CHECK(harness.registry.find_definition("inbox") != nullptr);
};

TEST_CASE("does_not_register_when_automation_runtime_is_absent") {
    orangutan::ToolRuntimeContext context{
        .agent_key = "default",
        .automation_runtime = nullptr,
    };
    orangutan::ToolRegistry registry;

    orangutan::tools::register_inbox_tool(registry, &context);

    CHECK(registry.find_definition("inbox") == nullptr);
};
