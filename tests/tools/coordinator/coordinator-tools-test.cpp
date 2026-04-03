#include "tools/coordinator/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"
#include "coordinator/coordinator-manager.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    TEST_CASE("Coordinator tools are not registered without context", "[tools][coordinator]") {
        ToolRegistry registry;
        register_coordinator_tools(registry, nullptr);

        auto defs = registry.definitions();
        CHECK(defs.empty());
    }

    TEST_CASE("Coordinator tools registration", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto defs = registry.definitions();

        // All coordinator tools are deferred, so definitions() returns empty.
        // But they should be findable and executable.

        SECTION("registers agent_spawn tool") {
            const auto *def = registry.find_definition("agent_spawn");
            REQUIRE(def != nullptr);
            CHECK(!def->description.empty());
            CHECK(def->input_schema.contains("properties"));
        }

        SECTION("registers agent_send_message tool") {
            const auto *def = registry.find_definition("agent_send_message");
            REQUIRE(def != nullptr);
            CHECK(!def->description.empty());
        }

        SECTION("registers agent_stop tool") {
            const auto *def = registry.find_definition("agent_stop");
            REQUIRE(def != nullptr);
            CHECK(!def->description.empty());
        }

        SECTION("has deferred tools") {
            CHECK(registry.has_deferred_tools());
        }

        manager.shutdown();
    }

    TEST_CASE("agent_spawn tool returns stub response", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("spawn-1", "agent_spawn",
                                               {
                                                   {"agent_key", "general-purpose"},
                                                   {"prompt", "test task"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        // Stub handler returns accepted=false since coordinator_manager is not wired up in the tool handler
        CHECK(json.contains("accepted"));
        CHECK(json.contains("error"));

        manager.shutdown();
    }

    TEST_CASE("agent_send_message tool returns stub response", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("msg-1", "agent_send_message",
                                               {
                                                   {"text", "hello agent"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json.contains("sent"));

        manager.shutdown();
    }

    TEST_CASE("agent_stop tool returns stub response", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("stop-1", "agent_stop",
                                               {
                                                   {"run_id", "run-12345-0"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json.contains("stopped"));

        manager.shutdown();
    }

} // namespace
