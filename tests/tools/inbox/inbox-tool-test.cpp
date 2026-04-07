#include "tools/inbox/inbox-tool.hpp"

#include "tools/registry/tool.hpp"
#include "automation/scheduler.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

    TEST_CASE("inbox_unknown_op_returns_exact_error") {
        const auto db_path = orangutan::testing::unique_test_db_path("inbox-tool", "inbox-tool-unknown-op.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_runtime = &runtime,
        };
        orangutan::ToolRegistry registry;
        orangutan::tools::register_inbox_tool(registry, &context);

        const auto result = registry.execute(orangutan::ToolUse("inbox-unknown-op", "inbox", {{"op", "noop"}, {"id", "item-1"}}));

        CHECK(result.content == "Error: unknown operation. Supported: list, ack, clear.");
    }

    TEST_CASE("inbox_ack_requires_id_with_exact_error") {
        const auto db_path = orangutan::testing::unique_test_db_path("inbox-tool", "inbox-tool-missing-id.db");
        orangutan::automation::Store store(db_path.string());
        orangutan::automation::Runtime runtime(store);

        orangutan::ToolRuntimeContext context{
            .agent_key = "default",
            .automation_runtime = &runtime,
        };
        orangutan::ToolRegistry registry;
        orangutan::tools::register_inbox_tool(registry, &context);

        const auto result = registry.execute(orangutan::ToolUse("inbox-ack-missing-id", "inbox", {{"op", "ack"}}));

        CHECK(result.content == "Error: id is required.");
    }

} // namespace

TEST_CASE("registers_when_automation_runtime_is_present") {
    const auto db_path = orangutan::testing::unique_test_db_path("inbox-tool", "inbox-tool-register.db");
    orangutan::automation::Store store(db_path.string());
    orangutan::automation::Runtime runtime(store);

    orangutan::ToolRuntimeContext context{
        .agent_key = "default",
        .automation_runtime = &runtime,
    };
    orangutan::ToolRegistry registry;
    orangutan::tools::register_inbox_tool(registry, &context);

    CHECK(registry.find_definition("inbox") != nullptr);
};
