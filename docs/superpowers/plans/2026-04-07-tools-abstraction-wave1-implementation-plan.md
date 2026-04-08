# Tools Abstraction Wave 1 Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `src/tools` internals to remove duplicated registration and op-dispatch boilerplate while preserving exact tool contracts and runtime behavior.

**Architecture:** Keep `ToolRegistry` and runtime loading semantics unchanged, then add only small registry-level helpers that collapse repeated shell code around registration, gating, and op routing. Migrate tool families in compatibility-first order: first lock runtime/tool-definition behavior with tests, then introduce narrowly scoped helpers, then move automation tools, collaboration tools, and memory aliases one family at a time.

**Tech Stack:** C++23, nlohmann_json, Catch2, xmake

**Spec:** `docs/superpowers/specs/2026-04-07-tools-abstraction-wave1-design.md`

---

## Planned File Structure

### New Files

- `src/tools/registry/op-tool-support.hpp` - tiny helpers for per-tool `op` input routing and high-frequency guard text.

### Modified Files

- `docs/superpowers/specs/2026-04-07-tools-abstraction-wave1-design.md` - approved design record.
- `src/tools/registry/schema-fragments.hpp` - add only exact-match reusable schema fragments.
- `src/tools/registry/tool-dispatch.hpp` - keep API stable; only adjust if helper support needs tiny additions.
- `src/tools/task/task-tool.cpp` - migrate repeated op routing and guard boilerplate.
- `src/tools/heartbeat/heartbeat-tool.cpp` - migrate repeated op routing and guard boilerplate.
- `src/tools/inbox/inbox-tool.cpp` - migrate repeated op routing and guard boilerplate.
- `src/tools/message-attachments/message-attachments-tool.cpp` - migrate repeated routing shell while preserving JSON envelopes.
- `src/tools/coordinator/agent-spawn-tool.cpp` - reduce registration/schema boilerplate.
- `src/tools/coordinator/agent-send-message-tool.cpp` - reduce registration/schema boilerplate.
- `src/tools/coordinator/agent-stop-tool.cpp` - reduce registration/schema boilerplate.
- `src/tools/coordinator/register.cpp` - keep family entry point stable while shrinking mechanical forwarding if possible.
- `src/tools/swarm/team-create-tool.cpp` - reduce registration/schema boilerplate.
- `src/tools/swarm/team-delete-tool.cpp` - reduce registration/schema boilerplate.
- `src/tools/swarm/register.cpp` - keep family entry point stable while shrinking mechanical forwarding if possible.
- `src/tools/internal.hpp` - remove helper code only if migration makes it redundant.
- `src/tools/memory/memory-tool.cpp` - add alias registration helper and preserve exact tool definitions.
- `tests/tools/registry/tool-abstraction-helpers-test.cpp` - helper contract tests.
- `tests/tools/registry/tool-registry-test.cpp` - deferred/filter/runtime behavior lock tests.
- `tests/bootstrap/runtime-agent-runtime-test.cpp` - coordinator-only and `tool_search` compatibility checks if coverage is missing.
- `tests/bootstrap/bootstrap-test.cpp` - builtin registration compatibility checks if coverage is missing.
- `tests/tools/task/task-tool-test.cpp` - exact `ToolDef` and runtime-context compatibility checks.
- `tests/tools/heartbeat/heartbeat-tool-test.cpp` - exact `ToolDef` and runtime-context compatibility checks.
- `tests/tools/inbox/inbox-tool-test.cpp` - exact `ToolDef` and runtime-context compatibility checks.
- `tests/tools/message-attachments/message-attachments-tool-test.cpp` - exact JSON error envelope and `ToolDef` checks.
- `tests/tools/coordinator/coordinator-tools-test.cpp` - registration/discovery compatibility checks if migration changes definitions.
- `tests/tools/swarm/swarm-tools-test.cpp` - registration/discovery compatibility checks if migration changes definitions.
- `tests/memory/memory-test.cpp` - alias tool presence and `ToolDef` compatibility checks.

---

## Chunk 1: Compatibility Baseline

### Task 1: Lock runtime registration and deferred visibility behavior before refactoring

**Files:**
- Modify: `tests/tools/registry/tool-registry-test.cpp`
- Modify: `tests/bootstrap/runtime-agent-runtime-test.cpp`
- Modify: `tests/bootstrap/bootstrap-test.cpp`
- Modify: `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- Modify: `tests/tools/inbox/inbox-tool-test.cpp`
- Modify: `tests/tools/message-attachments/message-attachments-tool-test.cpp`

- [ ] **Step 1: Add a failing test for deferred visibility vs direct execution**

```cpp
TEST_CASE("deferred tools remain directly executable before discovery", "[tools][registry][compat]") {
    orangutan::tools::ToolRegistry registry;
    registry.register_tool({
        .definition = {.name = "hidden_demo", .description = "hidden demo", .input_schema = {{"type", "object"}}},
        .execute = [](const nlohmann::json &) { return std::string{"ok"}; },
        .deferred = true,
    });

    CHECK(registry.definitions().empty());
    REQUIRE(registry.find_definition("hidden_demo") != nullptr);

    const auto result = registry.execute(orangutan::ToolUse("call-1", "hidden_demo", nlohmann::json::object()));
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "ok");
}
```

- [ ] **Step 2: Add or extend a coordinator-only runtime test using the existing `RuntimeAgentRuntimeHarness` fixture**

```cpp
TEST_CASE("coordinator runtime discovers deferred tools without registering tool_search", "[bootstrap][tools][compat]") {
    RuntimeAgentRuntimeHarness harness;
    auto input = harness.make_input();
    input.coordinator_mode = true;
    input.team_agents = {"explorer", "planner"};

    auto runtime = build_agent_runtime(input);
    const auto definitions = runtime.tools.definitions();

    CHECK(definitions.size() == 3);
    CHECK(orangutan::testing::has_tool_named(definitions, "agent_spawn"));
    CHECK(orangutan::testing::has_tool_named(definitions, "agent_send_message"));
    CHECK(orangutan::testing::has_tool_named(definitions, "agent_stop"));
    CHECK_FALSE(orangutan::testing::has_tool_named(definitions, "tool_search"));
    CHECK_FALSE(orangutan::testing::has_tool_named(definitions, "shell"));
}
```

- [ ] **Step 3: Add a failing test that locks the `definition_filter -> find_tool() -> execution_guard` coupling**

```cpp
TEST_CASE("definition filter still affects execution guard tool lookup", "[tools][registry][compat]") {
    orangutan::tools::ToolRegistry registry;
    bool permission_checker_called = false;

    registry.register_tool({
        .definition = {.name = "mutating_demo", .description = "mutating demo", .input_schema = {{"type", "object"}}},
        .read_only = false,
        .check_permissions = [&permission_checker_called](const auto &, const auto &) {
            permission_checker_called = true;
            return orangutan::permissions::PermissionResult::allow();
        },
        .execute = [](const nlohmann::json &) { return std::string{"ok"}; },
    });

    registry.set_definition_filter([](const orangutan::tools::Tool &tool) {
        return tool.definition.name != "mutating_demo";
    });

    registry.set_execution_guard([&registry, &permission_checker_called](const orangutan::ToolUse &call) -> std::optional<orangutan::ToolResult> {
        const auto *tool = registry.find_tool(call.name);
        if (tool == nullptr) {
            return orangutan::ToolResult{call.id, "filtered before permission lookup", true};
        }
        static_cast<void>(tool->check_permissions(call, {}));
        return std::nullopt;
    });

    const auto result = registry.execute(orangutan::ToolUse("filtered-call", "mutating_demo", nlohmann::json::object()));
    CHECK(result.is_error);
    CHECK(result.content == "filtered before permission lookup");
    CHECK_FALSE(permission_checker_called);
}
```

- [ ] **Step 4: Add missing live-context compatibility tests for `heartbeat` and `inbox`, and confirm `message_attachments` keeps its channel gate and JSON envelopes**

```cpp
TEST_CASE("heartbeat_registered_tool_reports_unavailable_context_at_execute_time") {
    // mirror the existing task test: register with runtime, set context.automation_runtime = nullptr,
    // execute {"op":"list"}, expect exact unavailable text.
}

TEST_CASE("inbox_registered_tool_reports_unavailable_context_at_execute_time") {
    // same pattern as heartbeat/task, expect exact inbox unavailable text.
}

TEST_CASE("heartbeat_and_inbox_are_not_registered_without_automation_runtime") {
    // build ToolRuntimeContext with agent_key only and assert both registrations remain absent.
}

TEST_CASE("message_attachments_definition_remains_channel_only") {
    // keep using the existing non-channel registration test and extend only if a definition assertion is missing.
}
```

- [ ] **Step 5: Run the compatibility targets and observe either immediate PASS or a narrowly scoped failure from the new assertions**

Run: `xmake run test-tools`
Expected: either PASS immediately or FAIL only in the newly added compatibility assertions.

Run: `xmake run test-bootstrap`
Expected: either PASS immediately or FAIL only in the newly added coordinator-only compatibility assertions.

- [ ] **Step 6: Implement only the missing tests and local setup, reusing the concrete harness patterns already present in each file**

```cpp
// Use RuntimeAgentRuntimeHarness in runtime-agent-runtime-test.cpp.
// Use inline ToolRuntimeContext + Store + Runtime setup in task/heartbeat/inbox tests.
// Use existing message-attachments test setup and exact JSON parsing style.
```

- [ ] **Step 7: Re-run the same targets and verify they pass with production code unchanged**

Run: `xmake run test-tools`
Expected: PASS

Run: `xmake run test-bootstrap`
Expected: PASS


### Task 2: Lock exact `ToolDef` compatibility for migrated tools

**Files:**
- Modify: `tests/tools/task/task-tool-test.cpp`
- Modify: `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- Modify: `tests/tools/inbox/inbox-tool-test.cpp`
- Modify: `tests/tools/message-attachments/message-attachments-tool-test.cpp`
- Modify: `tests/memory/memory-test.cpp`

- [ ] **Step 1: Add a failing `task` definition snapshot-style assertion**

```cpp
TEST_CASE("task tool definition remains exact", "[tools][task][definition]") {
    orangutan::tools::ToolRegistry registry;
    const auto db_path = orangutan::testing::unique_test_db_path("task-tool", "task-tool-definition.db");
    orangutan::automation::Store store(db_path.string());
    orangutan::automation::Runtime runtime(store);
    orangutan::ToolRuntimeContext context{.agent_key = "default", .automation_runtime = &runtime};
    orangutan::tools::register_task_tool(registry, &context);

    const auto *definition = registry.find_definition("task");
    REQUIRE(definition != nullptr);
    CHECK(definition->description == "Manage precise scheduled tasks for the current agent.");
    CHECK(definition->input_schema == nlohmann::json{
        {"type", "object"},
        {"properties", {
            {"op", {{"type", "string"}, {"enum", nlohmann::json::array({"add", "update", "remove", "list", "run"})}}},
            {"id", {{"type", "string"}}},
            {"name", {{"type", "string"}}},
            {"schedule_kind", {{"type", "string"}, {"enum", nlohmann::json::array({"at", "cron"})}}},
            {"schedule", {{"type", "string"}}},
            {"prompt", {{"type", "string"}}},
            {"notes", {{"type", "string"}}},
            {"enabled", {{"type", "boolean"}}},
            {"delivery_mode", {{"type", "string"}, {"enum", nlohmann::json::array({"silent", "notify"})}}},
            {"targets", {{"type", "array"}, {"items", {{"type", "string"}}}}},
        }},
        {"required", nlohmann::json::array({"op"})},
    });
}
```

- [ ] **Step 2: Add equivalent failing exact-definition assertions for `heartbeat`, `inbox`, and `message_attachments`**

```cpp
// Use the same inline Store + Runtime + ToolRuntimeContext setup already present in the current files.
// Include full expected JSON, including integer minimum for message_attachments.index.
```

- [ ] **Step 3: Add a failing test that all memory alias pairs still exist with exact names and current expected ToolDefs**

```cpp
TEST_CASE("memory alias tools remain registered with exact current definitions", "[memory][tools][definition]") {
    orangutan::tools::ToolRegistry registry;
    MemoryStoreHarness harness;
    MemoryStore store(harness.db_path());
    RuntimeMemory runtime_memory(store);
    orangutan::tools::register_builtin_memory_tools(registry, runtime_memory);

    REQUIRE(registry.find_definition("remember") != nullptr);
    REQUIRE(registry.find_definition("memory_store") != nullptr);
    REQUIRE(registry.find_definition("recall") != nullptr);
    REQUIRE(registry.find_definition("memory_recall") != nullptr);
    REQUIRE(registry.find_definition("forget") != nullptr);
    REQUIRE(registry.find_definition("memory_forget") != nullptr);

    // Lock each alias tool's current contract rather than assuming alias pairs are identical.
    CHECK(registry.find_definition("remember")->description.contains("Store a durable fact"));
    CHECK(registry.find_definition("memory_store")->description.contains("Plugin-style alias for remember"));
    CHECK(registry.find_definition("recall")->input_schema == registry.find_definition("memory_recall")->input_schema);
    CHECK(registry.find_definition("forget")->input_schema == registry.find_definition("memory_forget")->input_schema);
}
```

- [ ] **Step 4: Run the full tool and memory suites to lock these exact-definition assertions against current production code**

Run: `xmake run test-tools`
Expected: either PASS immediately or FAIL only in the newly added exact-definition assertions.

Run: `xmake run test-memory`
Expected: either PASS immediately or FAIL only in the new alias-definition assertions.

- [ ] **Step 5: Implement the tests only, using full expected JSON values copied from current production definitions**

```cpp
// Do not introduce helper builders in tests; compare against full explicit JSON.
```

- [ ] **Step 6: Re-run the targeted tests and verify they pass**

Run: `xmake run test-tools`
Expected: PASS

Run: `xmake run test-memory`
Expected: PASS

- [ ] **Step 7: Commit the baseline test lock**

```bash
git add tests/tools/registry/tool-registry-test.cpp tests/bootstrap/runtime-agent-runtime-test.cpp tests/bootstrap/bootstrap-test.cpp tests/tools/task/task-tool-test.cpp tests/tools/heartbeat/heartbeat-tool-test.cpp tests/tools/inbox/inbox-tool-test.cpp tests/tools/message-attachments/message-attachments-tool-test.cpp tests/memory/memory-test.cpp
git commit -m "test(tools): lock runtime and tool definition compatibility"
```

---

## Chunk 2: Helper Foundation

### Task 3: Add narrowly scoped helper tests for op-tool support and schema fragments

**Files:**
- Modify: `tests/tools/registry/tool-abstraction-helpers-test.cpp`

- [ ] **Step 1: Add failing tests for `routed_input_with_default_op` preserving explicit and default `op` values**

```cpp
TEST_CASE("op_tool_support: routed_input_with_default_op preserves explicit op", "[tools][registry][abstractions]") {
    const auto input = nlohmann::json{{"op", "list"}, {"name", "demo"}};
    const auto routed = orangutan::tools::routed_input_with_default_op(input, "fallback");
    CHECK(routed.at("op") == "list");
    CHECK(routed.at("name") == "demo");
}

TEST_CASE("op_tool_support: routed_input_with_default_op injects provided default", "[tools][registry][abstractions]") {
    const auto routed = orangutan::tools::routed_input_with_default_op(nlohmann::json::object(), "list");
    CHECK(routed.at("op") == "list");
}
```

- [ ] **Step 2: Add failing tests for common guard helpers and message-unwrapping helper with exact outputs**

```cpp
TEST_CASE("op_tool_support: require_id_or_name returns exact compatibility text", "[tools][registry][abstractions]") {
    const auto result = orangutan::tools::require_id_or_name(nlohmann::json::object());
    REQUIRE(result.has_value());
    CHECK(*result == "Error: id or name is required.");
}

TEST_CASE("op_tool_support: require_id returns exact compatibility text", "[tools][registry][abstractions]") {
    const auto result = orangutan::tools::require_id(nlohmann::json::object());
    REQUIRE(result.has_value());
    CHECK(*result == "Error: id is required.");
}

TEST_CASE("op_tool_support: dispatch_message returns tool_dispatch message unchanged", "[tools][registry][abstractions]") {
    const auto output = orangutan::tools::dispatch_message(
        orangutan::tools::tool_dispatch().on("list", [](const nlohmann::json &) {
            return orangutan::tools::tool_dispatch::response{"ok"};
        }),
        nlohmann::json{{"op", "list"}});

    CHECK(output == "ok");
}
```

- [ ] **Step 3: Add failing schema fragment tests only for fragments that will be reused exactly**

```cpp
TEST_CASE("schema_fragments: index/string/boolean fields preserve exact JSON", "[tools][registry][abstractions]") {
    const auto schema = orangutan::tools::schema_fragments::non_negative_index_field();
    CHECK(schema == nlohmann::json{{"type", "integer"}, {"minimum", 0}});

    CHECK(orangutan::tools::schema_fragments::string_field() == nlohmann::json{{"type", "string"}});
    CHECK(orangutan::tools::schema_fragments::boolean_field() == nlohmann::json{{"type", "boolean"}});
}
```

- [ ] **Step 4: Run helper tests and confirm failure before helper implementation**

Run: `xmake run test-tools -- "[tools][registry][abstractions]"`
Expected: FAIL with missing helper declarations.

- [ ] **Step 5: Re-run helper tests once the test file compiles and verify failure is now limited to missing production helpers**

Run: `xmake run test-tools -- "[tools][registry][abstractions]"`
Expected: FAIL with missing production helper implementations.


### Task 4: Implement helper foundation with no tool migrations yet

**Files:**
- Create: `src/tools/registry/op-tool-support.hpp`
- Modify: `src/tools/registry/schema-fragments.hpp`
- Modify: `tests/tools/registry/tool-abstraction-helpers-test.cpp`

- [ ] **Step 1: Implement `op-tool-support.hpp` with only tiny reusable helpers**

```cpp
namespace orangutan::tools {

[[nodiscard]] inline nlohmann::json routed_input_with_default_op(const nlohmann::json &input, std::string_view default_op) {
    auto routed = input;
    routed["op"] = input.value("op", std::string{default_op});
    return routed;
}

[[nodiscard]] inline std::optional<std::string> require_id_or_name(const nlohmann::json &request) {
    const auto id_or_name = request.value("id", request.value("name", ""));
    if (id_or_name.empty()) {
        return std::string{"Error: id or name is required."};
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> require_id(const nlohmann::json &request) {
    const auto id = request.value("id", "");
    if (id.empty()) {
        return std::string{"Error: id is required."};
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string dispatch_message(const ToolDispatch &dispatch, const nlohmann::json &input) {
    return dispatch.run(input).message;
}

} // namespace orangutan::tools
```

- [ ] **Step 2: Extend `schema-fragments.hpp` only with exact-match reusable fields**

```cpp
[[nodiscard]] inline nlohmann::json string_field() {
    return {{"type", "string"}};
}

[[nodiscard]] inline nlohmann::json boolean_field() {
    return {{"type", "boolean"}};
}

[[nodiscard]] inline nlohmann::json non_negative_index_field() {
    return {{"type", "integer"}, {"minimum", 0}};
}
```

- [ ] **Step 3: Run helper tests and verify they pass without touching any tool implementation**

Run: `xmake run test-tools -- "[tools][registry][abstractions]"`
Expected: PASS

- [ ] **Step 4: Run the broader registry test target to ensure no regression in foundational helpers**

Run: `xmake run test-tools -- "[tools][registry]"`
Expected: PASS

- [ ] **Step 5: Commit the helper foundation**

```bash
git add src/tools/registry/op-tool-support.hpp src/tools/registry/schema-fragments.hpp tests/tools/registry/tool-abstraction-helpers-test.cpp
git commit -m "refactor(tools): add minimal op tool helper foundation"
```

---

## Chunk 3: Automation Tool Migration

### Task 5: Refactor `task-tool.cpp` to use the tiny helpers only

**Files:**
- Modify: `src/tools/task/task-tool.cpp`
- Modify: `src/tools/heartbeat/heartbeat-tool.cpp`
- Modify: `src/tools/inbox/inbox-tool.cpp`
- Test: `tests/tools/task/task-tool-test.cpp`
- Test: `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- Test: `tests/tools/inbox/inbox-tool-test.cpp`

- [ ] **Step 1: Run the full tools suite as baseline before touching automation tools**

Run: `xmake run test-tools`
Expected: PASS

- [ ] **Step 2: Replace repeated `op` routing shell in `task-tool.cpp` using the tiny helpers only**

```cpp
const auto result = tool_dispatch()
    .unknown_op_error("Error: unknown operation. Supported: add, update, remove, list, run.")
    // handlers unchanged
    .run(routed_input_with_default_op(input, ""));

return result.message;
```

- [ ] **Step 3: Replace repeated `id or name` guard branches with the helper where behavior is identical**

```cpp
if (const auto missing = require_id_or_name(request); missing.has_value()) {
    return tool_dispatch::response{*missing, true};
}
```

- [ ] **Step 4: Re-run the full tools suite and verify `task` compatibility tests remain green**

Run: `xmake run test-tools`
Expected: PASS

- [ ] **Step 5: Commit the `task` migration**

```bash
git add src/tools/task/task-tool.cpp tests/tools/task/task-tool-test.cpp
git commit -m "refactor(tools): reduce task routing boilerplate"
```


### Task 6: Refactor `heartbeat-tool.cpp` to use the tiny helpers only

**Files:**
- Modify: `src/tools/heartbeat/heartbeat-tool.cpp`
- Test: `tests/tools/heartbeat/heartbeat-tool-test.cpp`

- [ ] **Step 1: Replace repeated routing shell in `heartbeat-tool.cpp` using the helper functions only**

```cpp
// Keep heartbeat parsing, active-hours handling, and runtime calls local.
// Share only routed-input construction and exact missing-id-or-name text.
```

- [ ] **Step 2: Re-run the full tools suite and verify heartbeat compatibility tests remain green**

Run: `xmake run test-tools`
Expected: PASS

- [ ] **Step 3: Commit the `heartbeat` migration**

```bash
git add src/tools/heartbeat/heartbeat-tool.cpp tests/tools/heartbeat/heartbeat-tool-test.cpp
git commit -m "refactor(tools): reduce heartbeat routing boilerplate"
```


### Task 7: Refactor `inbox-tool.cpp` to use the tiny helpers only

**Files:**
- Modify: `src/tools/inbox/inbox-tool.cpp`
- Test: `tests/tools/inbox/inbox-tool-test.cpp`

- [ ] **Step 1: Replace repeated routing shell in `inbox-tool.cpp` using the helper functions only**

```cpp
// Keep inbox list/ack/clear behavior local.
// Share only routed-input construction and exact missing-id text.
```

- [ ] **Step 2: Re-run the full tools suite and verify inbox compatibility tests remain green**

Run: `xmake run test-tools`
Expected: PASS

- [ ] **Step 3: Commit the `inbox` migration**

```bash
git add src/tools/inbox/inbox-tool.cpp tests/tools/inbox/inbox-tool-test.cpp
git commit -m "refactor(tools): reduce inbox routing boilerplate"
```


### Task 8: Run combined automation compatibility coverage

Run: `xmake run test-tools`
Expected: PASS


### Task 9: Refactor `message_attachments` without changing JSON error envelopes

**Files:**
- Modify: `src/tools/message-attachments/message-attachments-tool.cpp`
- Test: `tests/tools/message-attachments/message-attachments-tool-test.cpp`

- [ ] **Step 1: Use the existing channel-only registration and JSON envelope tests as the pre-change oracle**

Run: `xmake run test-tools`
Expected: PASS

- [ ] **Step 2: Switch only the routed-input shell to the helper while preserving `default_op = list`**

```cpp
const auto result = tool_dispatch()
    .unknown_op_error_formatter([](std::string_view) {
        return R"({"error":"unknown operation. Supported: list, download."})";
    })
    // handlers unchanged
    .run(routed_input_with_default_op(input, "list"));
```

- [ ] **Step 3: Reuse exact-match schema fragments only where the emitted JSON is identical**

```cpp
{"index", schema_fragments::non_negative_index_field()},
{"target_path", schema_fragments::string_field()},
```

- [ ] **Step 4: Verify JSON payloads, missing-callback behavior, and exact definition tests remain unchanged**

Run: `xmake run test-tools`
Expected: PASS

- [ ] **Step 5: Commit the `message_attachments` migration**

```bash
git add src/tools/message-attachments/message-attachments-tool.cpp tests/tools/message-attachments/message-attachments-tool-test.cpp
git commit -m "refactor(tools): preserve message attachment routing semantics"
```

---

## Chunk 4: Collaboration Tool Registration Migration

### Task 10: Refactor coordinator registration boilerplate

**Files:**
- Modify: `src/tools/coordinator/agent-spawn-tool.cpp`
- Modify: `src/tools/coordinator/agent-send-message-tool.cpp`
- Modify: `src/tools/coordinator/agent-stop-tool.cpp`
- Modify: `src/tools/coordinator/register.cpp`
- Modify: `src/tools/internal.hpp`
- Test: `tests/tools/coordinator/coordinator-tools-test.cpp`
- Test: `tests/bootstrap/runtime-agent-runtime-test.cpp`

- [ ] **Step 1: Add exact `ToolDef` assertions for `agent_spawn`, `agent_send_message`, and `agent_stop` if they are missing**

```cpp
// Assert exact description and full input_schema JSON for each coordinator tool.
// If exact schema equality becomes awkward to express, copy the current literal from production code.
```

- [ ] **Step 2: Run coordinator tests and bootstrap coordinator-only runtime tests as baseline**

Run: `xmake run test-tools -- "[tools][coordinator]"`
Expected: PASS

Run: `xmake run test-bootstrap`
Expected: PASS

- [ ] **Step 3: Replace repeated schema literals with exact-match fragments only when `definition.input_schema == expected_json` still holds**

```cpp
auto schema = schema_fragments::object_with_required(
    {
        {"name", schema_fragments::string_field()},
        {"prompt", schema_fragments::string_field()},
    },
    {"name", "prompt"});

// If any fragment changes the resulting JSON, keep the original explicit schema literal.
```

- [ ] **Step 4: Collapse purely mechanical coordinator registration wrappers while keeping public entry points unchanged**

```cpp
// `register_coordinator_tools(...)` signature stays unchanged.
// Remove helper indirection only if a file becomes simpler after direct builder usage.
```

- [ ] **Step 5: Re-run coordinator tests and coordinator-only runtime coverage**

Run: `xmake run test-tools -- "[tools][coordinator]"`
Expected: PASS

Run: `xmake run test-bootstrap`
Expected: PASS

- [ ] **Step 6: Commit the coordinator registration migration**

```bash
git add src/tools/coordinator/agent-spawn-tool.cpp src/tools/coordinator/agent-send-message-tool.cpp src/tools/coordinator/agent-stop-tool.cpp src/tools/coordinator/register.cpp src/tools/internal.hpp tests/tools/coordinator/coordinator-tools-test.cpp tests/bootstrap/runtime-agent-runtime-test.cpp
git commit -m "refactor(tools): simplify coordinator tool registration"
```


### Task 11: Refactor swarm registration boilerplate

**Files:**
- Modify: `src/tools/swarm/team-create-tool.cpp`
- Modify: `src/tools/swarm/team-delete-tool.cpp`
- Modify: `src/tools/swarm/register.cpp`
- Modify: `src/tools/internal.hpp`
- Test: `tests/tools/swarm/swarm-tools-test.cpp`
- Test: `tests/bootstrap/runtime-agent-runtime-test.cpp`

- [ ] **Step 1: Add exact `ToolDef` assertions for `team_create` and `team_delete` if they are missing**

```cpp
// Assert exact description and full input_schema JSON for both swarm tools.
```

- [ ] **Step 2: Run swarm tests and bootstrap coordinator-related runtime tests as baseline**

Run: `xmake run test-tools -- "[tools][swarm]"`
Expected: PASS

Run: `xmake run test-bootstrap`
Expected: PASS

- [ ] **Step 3: Replace repeated schema literals with exact-match fragments only when full `ToolDef` equality is preserved**

```cpp
// Abort fragment reuse for any field where the generated JSON differs from the current definition.
```

- [ ] **Step 4: Collapse purely mechanical swarm registration wrappers while keeping public entry points unchanged**

```cpp
// `register_swarm_tools(...)` signature stays unchanged.
```

- [ ] **Step 5: Re-run swarm tests and bootstrap coverage**

Run: `xmake run test-tools -- "[tools][swarm]"`
Expected: PASS

Run: `xmake run test-bootstrap`
Expected: PASS

- [ ] **Step 6: Commit the swarm registration migration**

```bash
git add src/tools/swarm/team-create-tool.cpp src/tools/swarm/team-delete-tool.cpp src/tools/swarm/register.cpp src/tools/internal.hpp tests/tools/swarm/swarm-tools-test.cpp tests/bootstrap/runtime-agent-runtime-test.cpp
git commit -m "refactor(tools): simplify swarm tool registration"
```

---

## Chunk 5: Memory Alias Consolidation and Final Verification

### Task 12: Add alias-registration helper in memory tools without changing definitions

**Files:**
- Modify: `src/tools/memory/memory-tool.cpp`
- Test: `tests/memory/memory-test.cpp`

- [ ] **Step 1: Run memory tests as baseline**

Run: `xmake run test-memory`
Expected: PASS

- [ ] **Step 2: Introduce a local helper that registers alias tools from full explicit `ToolDef` values**

```cpp
namespace {

void register_memory_alias_tool(orangutan::tools::ToolRegistry &registry, orangutan::tools::ToolDef definition,
                                std::function<std::string(const nlohmann::json &)> execute) {
    registry.register_tool({
        .definition = std::move(definition),
        .execute = std::move(execute),
        .deferred = true,
    });
}

} // namespace
```

- [ ] **Step 3: Replace repeated alias registrations while preserving exact descriptions and schemas**

```cpp
// Keep each alias definition explicit.
// Share only the final `registry.register_tool(...)` shell.
```

- [ ] **Step 4: Re-run memory tests and exact definition assertions**

Run: `xmake run test-memory`
Expected: PASS

Run: `xmake run test-tools -- "[definition]"`
Expected: PASS

- [ ] **Step 5: Commit the memory alias consolidation**

```bash
git add src/tools/memory/memory-tool.cpp tests/memory/memory-test.cpp
git commit -m "refactor(tools): consolidate memory alias registration"
```


### Task 13: Run full verification for the wave and clean up only redundant code

**Files:**
- Modify: `src/tools/internal.hpp`
- Modify: `src/tools/registry/schema-fragments.hpp`
- Modify: `src/tools/coordinator/register.cpp`
- Modify: `src/tools/swarm/register.cpp`
- Modify: `src/tools/memory/memory-tool.cpp`
- Modify: any already-touched source/test file only if verification identifies a concrete redundant block there

- [ ] **Step 1: Run the full tools test target**

Run: `xmake run test-tools`
Expected: PASS

- [ ] **Step 2: Run bootstrap and memory regression coverage**

Run: `xmake run test-bootstrap`
Expected: PASS

Run: `xmake run test-memory`
Expected: PASS

- [ ] **Step 3: Identify the exact redundant block, clean only that block, and list the touched file path in the commit message notes**

```cpp
// Remove only code made obviously redundant by the migration.
// Do not widen scope after green tests.
```

- [ ] **Step 4: Re-run the full regression set after cleanup**

Run: `xmake run test-tools`
Expected: PASS

Run: `xmake run test-bootstrap`
Expected: PASS

Run: `xmake run test-memory`
Expected: PASS

- [ ] **Step 5: Review `git status --short` and stage only the exact known wave-one files**

```bash
git status --short
git add docs/superpowers/specs/2026-04-07-tools-abstraction-wave1-design.md docs/superpowers/plans/2026-04-07-tools-abstraction-wave1-implementation-plan.md src/tools/registry/op-tool-support.hpp src/tools/registry/schema-fragments.hpp src/tools/task/task-tool.cpp src/tools/heartbeat/heartbeat-tool.cpp src/tools/inbox/inbox-tool.cpp src/tools/message-attachments/message-attachments-tool.cpp src/tools/coordinator/agent-spawn-tool.cpp src/tools/coordinator/agent-send-message-tool.cpp src/tools/coordinator/agent-stop-tool.cpp src/tools/coordinator/register.cpp src/tools/swarm/team-create-tool.cpp src/tools/swarm/team-delete-tool.cpp src/tools/swarm/register.cpp src/tools/internal.hpp src/tools/memory/memory-tool.cpp tests/tools/registry/tool-abstraction-helpers-test.cpp tests/tools/registry/tool-registry-test.cpp tests/tools/task/task-tool-test.cpp tests/tools/heartbeat/heartbeat-tool-test.cpp tests/tools/inbox/inbox-tool-test.cpp tests/tools/message-attachments/message-attachments-tool-test.cpp tests/tools/coordinator/coordinator-tools-test.cpp tests/tools/swarm/swarm-tools-test.cpp tests/bootstrap/runtime-agent-runtime-test.cpp tests/bootstrap/bootstrap-test.cpp tests/memory/memory-test.cpp
```

- [ ] **Step 6: Commit the final wave cleanup**

```bash
git commit -m "refactor(tools): complete abstraction wave one"
```
