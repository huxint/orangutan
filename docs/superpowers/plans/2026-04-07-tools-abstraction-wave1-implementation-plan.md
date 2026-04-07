# Tools Abstraction Wave 1 Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `src/tools` abstraction internals to remove repeated registration/dispatch boilerplate while keeping tool names, JSON contracts, and behavior unchanged.

**Architecture:** Add small registry-level helpers (`tool-spec-builder`, `contextual-tool-group`, `tool-dispatch`, `schema-fragments`) and migrate coordinator/swarm plus automation-style tools onto them incrementally. Preserve current `ToolRegistry` execution path and deferred-tool semantics; only replace repetitive construction and routing code.

**Tech Stack:** C++23, nlohmann_json, Catch2, xmake

**Spec:** `docs/superpowers/specs/2026-04-07-tools-abstraction-wave1-design.md`

---

## Planned File Structure

### New Files

- `src/tools/registry/tool-spec-builder.hpp` - internal fluent builder for `Tool` construction.
- `src/tools/registry/contextual-tool-group.hpp` - gate-aware grouped registration helper.
- `src/tools/registry/tool-dispatch.hpp` - reusable op-dispatch helper returning compatible error text.
- `src/tools/registry/schema-fragments.hpp` - common JSON schema fragments used by migrated tools.
- `tests/tools/registry/tool-abstraction-helpers-test.cpp` - focused tests for new registry abstractions.

### Modified Files (Migration Scope)

- `src/tools/coordinator/agent-spawn-tool.cpp`
- `src/tools/coordinator/agent-send-message-tool.cpp`
- `src/tools/coordinator/agent-stop-tool.cpp`
- `src/tools/swarm/team-create-tool.cpp`
- `src/tools/swarm/team-delete-tool.cpp`
- `src/tools/task/task-tool.cpp`
- `src/tools/heartbeat/heartbeat-tool.cpp`
- `src/tools/inbox/inbox-tool.cpp`
- `src/tools/message-attachments/message-attachments-tool.cpp`

### Modified Files (Tests / Contract Lock)

- `tests/tools/coordinator/coordinator-tools-test.cpp`
- `tests/tools/swarm/swarm-tools-test.cpp`
- `tests/tools/task/task-tool-test.cpp`
- `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- `tests/tools/inbox/inbox-tool-test.cpp`
- `tests/tools/message-attachments/message-attachments-tool-test.cpp`
- `tests/tools/registry/tool-registry-test.cpp`

---

## Chunk 1: Registry Abstraction Foundation

### Task 1: Add failing contract tests for helper abstractions

**Files:**
- Create: `tests/tools/registry/tool-abstraction-helpers-test.cpp`
- Test: `tests/tools/registry/tool-abstraction-helpers-test.cpp`

- [ ] **Step 1: Write failing tests for `tool-spec-builder` contract and passthrough flags**

```cpp
TEST_CASE("tool_spec_builder builds executable tool with passthrough flags", "[tools][registry][abstractions]") {
    auto tool = make_tool_spec_builder()
                    .name("demo")
                    .description("demo tool")
                    .input_schema({{"type", "object"}})
                    .read_only(true)
                    .deferred(true)
                    .check_permissions([](const ToolUse &, const ToolPermissionContext &) {
                        return PermissionResult::allow();
                    })
                    .execute([](const nlohmann::json &) { return std::string{"ok"}; })
                    .build();

    REQUIRE(tool.definition.name == "demo");
    CHECK(tool.execute != nullptr);
    CHECK(tool.read_only);
    CHECK(tool.deferred);
    CHECK(tool.check_permissions != nullptr);
}

TEST_CASE("tool_spec_builder rejects missing execute endpoints", "[tools][registry][abstractions]") {
    CHECK_THROWS(make_tool_spec_builder()
                     .name("invalid")
                     .description("invalid")
                     .input_schema({{"type", "object"}})
                     .build());
}
```

- [ ] **Step 2: Run test target to confirm failure**

Run: `xmake run test-tools -- "[tools][registry][abstractions]"`
Expected: FAIL with missing helper symbols/includes.

- [ ] **Step 3: Write failing tests for `contextual-tool-group` gate behavior and isolation**

```cpp
TEST_CASE("contextual_tool_group skips registration when gate fails", "[tools][registry][abstractions]") {
    ToolRegistry registry;
    register_contextual_tool_group(registry, nullptr,
        {context_gate::require_tool_context},
        [] (ToolRegistry &out) {
            out.register_tool({.definition = {.name = "gated", .description = "gated"},
                               .execute = [](const nlohmann::json &) { return std::string{"ok"}; }});
        });

    CHECK(registry.find_definition("gated") == nullptr);
}

TEST_CASE("contextual_tool_group gate failure does not block unrelated registration", "[tools][registry][abstractions]") {
    ToolRegistry registry;

    registry.register_tool({.definition = {.name = "always", .description = "always"},
                            .execute = [](const nlohmann::json &) { return std::string{"ok"}; }});

    register_contextual_tool_group(registry, nullptr,
        {context_gate::require_tool_context},
        [] (ToolRegistry &out) {
            out.register_tool({.definition = {.name = "gated", .description = "gated"},
                               .execute = [](const nlohmann::json &) { return std::string{"ok"}; }});
        });

    CHECK(registry.find_definition("always") != nullptr);
    CHECK(registry.find_definition("gated") == nullptr);
}

TEST_CASE("contextual_tool_group requires automation runtime", "[tools][registry][abstractions]") {
    ToolRegistry registry_without_runtime;
    ToolRuntimeContext without_runtime{};
    register_contextual_tool_group(registry_without_runtime, &without_runtime,
        {context_gate::require_automation_runtime},
        [] (ToolRegistry &out) {
            out.register_tool({.definition = {.name = "automation_only", .description = "automation_only"},
                               .execute = [](const nlohmann::json &) { return std::string{"ok"}; }});
        });
    CHECK(registry_without_runtime.find_definition("automation_only") == nullptr);

    ToolRegistry registry_with_runtime;
    const auto db_path = orangutan::testing::unique_test_db_path("tool-abstractions", "context-gate.db");
    orangutan::automation::Store store(db_path.string());
    orangutan::automation::Runtime runtime(store);
    ToolRuntimeContext with_runtime{.automation_runtime = &runtime};
    register_contextual_tool_group(registry_with_runtime, &with_runtime,
        {context_gate::require_automation_runtime},
        [] (ToolRegistry &out) {
            out.register_tool({.definition = {.name = "automation_only", .description = "automation_only"},
                               .execute = [](const nlohmann::json &) { return std::string{"ok"}; }});
        });
    CHECK(registry_with_runtime.find_definition("automation_only") != nullptr);
    std::filesystem::remove_all(db_path.parent_path());
}

TEST_CASE("contextual_tool_group requires channel origin", "[tools][registry][abstractions]") {
    ToolRegistry registry_cli;
    ToolRuntimeContext cli_ctx{.runtime_origin = base::origin::cli};
    register_contextual_tool_group(registry_cli, &cli_ctx,
        {context_gate::require_channel_origin},
        [] (ToolRegistry &out) {
            out.register_tool({.definition = {.name = "channel_only", .description = "channel_only"},
                               .execute = [](const nlohmann::json &) { return std::string{"ok"}; }});
        });
    CHECK(registry_cli.find_definition("channel_only") == nullptr);

    ToolRegistry registry_channel;
    ToolRuntimeContext channel_ctx{.runtime_origin = base::origin::channel};
    register_contextual_tool_group(registry_channel, &channel_ctx,
        {context_gate::require_channel_origin},
        [] (ToolRegistry &out) {
            out.register_tool({.definition = {.name = "channel_only", .description = "channel_only"},
                               .execute = [](const nlohmann::json &) { return std::string{"ok"}; }});
        });
    CHECK(registry_channel.find_definition("channel_only") != nullptr);
}
```

- [ ] **Step 4: Write failing tests for `tool-dispatch` unknown-op and missing-op paths**

```cpp
TEST_CASE("tool_dispatch returns configured unknown-op error", "[tools][registry][abstractions]") {
    const auto output = dispatch_tool_op(
        nlohmann::json{{"op", "noop"}},
        {
            {"list", [] { return std::string{"ok"}; }},
        },
        [] { return std::string{"Error: unknown operation. Supported: list."}; });

    CHECK(output == "Error: unknown operation. Supported: list.");
}

TEST_CASE("tool_dispatch routes known op to matching handler", "[tools][registry][abstractions]") {
    const auto output = dispatch_tool_op(
        nlohmann::json{{"op", "list"}},
        {
            {"list", [] { return std::string{"ok"}; }},
        },
        [] { return std::string{"Error: unknown operation. Supported: list."}; });

    CHECK(output == "ok");
}

TEST_CASE("tool_dispatch handles missing op with formatter error", "[tools][registry][abstractions]") {
    const auto output = dispatch_tool_op(
        nlohmann::json::object(),
        {
            {"list", [] { return std::string{"ok"}; }},
        },
        [] { return std::string{"Error: unknown operation. Supported: list."}; });

    CHECK(output == "Error: unknown operation. Supported: list.");
}
```

- [ ] **Step 5: Write failing test for `schema-fragments` composition**

```cpp
TEST_CASE("schema_fragments builds op+id object schema", "[tools][registry][abstractions]") {
    const auto schema = schema_fragments::object_with_required(
        {schema_fragments::op_enum({"list", "remove"}), schema_fragments::id_field()},
        {"op"});

    CHECK(schema.at("type") == "object");
    CHECK(schema.at("properties").contains("op"));
    CHECK(schema.at("properties").contains("id"));
}

TEST_CASE("schema_fragments provides empty object and delivery fields", "[tools][registry][abstractions]") {
    const auto empty = schema_fragments::empty_object_schema();
    CHECK(empty.at("type") == "object");

    const auto delivery_mode = schema_fragments::delivery_mode_field();
    CHECK(delivery_mode.at("type") == "string");
    CHECK(delivery_mode.at("enum").is_array());

    const auto delivery_targets = schema_fragments::delivery_targets_field();
    CHECK(delivery_targets.at("type") == "array");
    CHECK(delivery_targets.at("items").at("type") == "string");
}
```

- [ ] **Step 6: Commit tests**

```bash
git add tests/tools/registry/tool-abstraction-helpers-test.cpp
git commit -m "test(tools): add failing contract tests for registry abstractions"
```

### Task 2: Implement `tool-spec-builder` and `schema-fragments`

**Files:**
- Create: `src/tools/registry/tool-spec-builder.hpp`
- Create: `src/tools/registry/schema-fragments.hpp`
- Test: `tests/tools/registry/tool-abstraction-helpers-test.cpp`

- [ ] **Step 1: Implement `tool-spec-builder.hpp` to satisfy tests and invariant checks**

```cpp
class ToolSpecBuilder {
public:
    auto &name(std::string_view value);
    auto &description(std::string_view value);
    auto &input_schema(nlohmann::json schema);
    auto &read_only(bool value = true);
    auto &deferred(bool value = true);
    auto &check_permissions(Tool::check_permissions_type fn);
    auto &execute(Tool::execute_type fn);
    auto &execute_rich(Tool::execute_rich_type fn);
    [[nodiscard]] Tool build() const;
    // build() throws if both execute and execute_rich are missing.
};
```

- [ ] **Step 2: Implement `schema-fragments.hpp` minimally to satisfy tests**

```cpp
namespace schema_fragments {
    [[nodiscard]] nlohmann::json empty_object_schema();
    [[nodiscard]] nlohmann::json id_field();
    [[nodiscard]] nlohmann::json op_enum(std::initializer_list<std::string_view> ops);
    [[nodiscard]] nlohmann::json delivery_mode_field();
    [[nodiscard]] nlohmann::json delivery_targets_field();
    [[nodiscard]] nlohmann::json object_with_required(std::span<const nlohmann::json> properties,
                                                      std::span<const std::string_view> required);
}
```

- [ ] **Step 3: Run abstraction tests**

Run: `xmake run test-tools -- "[tools][registry][abstractions]"`
Expected: PASS for current implemented cases.

- [ ] **Step 4: Run registry compatibility tests**

Run: `xmake run test-tools -- "[tools][registry]"`
Expected: PASS (no `ToolRegistry` baseline regressions).

- [ ] **Step 5: Commit helper implementation**

```bash
git add src/tools/registry/tool-spec-builder.hpp src/tools/registry/schema-fragments.hpp tests/tools/registry/tool-abstraction-helpers-test.cpp
git commit -m "refactor(tools): add tool spec builder and schema fragments"
```

### Task 3: Implement `contextual-tool-group` and `tool-dispatch`

**Files:**
- Create: `src/tools/registry/contextual-tool-group.hpp`
- Create: `src/tools/registry/tool-dispatch.hpp`
- Test: `tests/tools/registry/tool-abstraction-helpers-test.cpp`

- [ ] **Step 1: Implement context gate enum and evaluator**

```cpp
enum class context_gate : base::u8 {
    require_tool_context,
    require_automation_runtime,
    require_channel_origin,
};

[[nodiscard]] bool gates_satisfied(std::span<const context_gate> gates, const ToolRuntimeContext *ctx);
```

- [ ] **Step 2: Implement grouped registration helper**

```cpp
template <class RegisterFn>
void register_contextual_tool_group(ToolRegistry &registry,
                                    const ToolRuntimeContext *ctx,
                                    std::span<const context_gate> gates,
                                    RegisterFn register_fn) {
    if (!gates_satisfied(gates, ctx)) {
        return;
    }
    register_fn(registry);
}
```

- [ ] **Step 3: Implement op-dispatch helper with configurable unknown-op formatter**

```cpp
template <class HandlerMap, class UnknownOpFormatter>
std::string dispatch_tool_op(const nlohmann::json &input,
                             const HandlerMap &handlers,
                             UnknownOpFormatter unknown_formatter);
```

- [ ] **Step 4: Run abstraction tests**

Run: `xmake run test-tools -- "[tools][registry][abstractions]"`
Expected: PASS.

- [ ] **Step 5: Run registry compatibility tests**

Run: `xmake run test-tools -- "[tools][registry]"`
Expected: PASS.

- [ ] **Step 6: Commit group/dispatch helpers**

```bash
git add src/tools/registry/contextual-tool-group.hpp src/tools/registry/tool-dispatch.hpp tests/tools/registry/tool-abstraction-helpers-test.cpp
git commit -m "refactor(tools): add contextual group and op dispatch helpers"
```

---

## Chunk 2: Tool Family Migrations

### Task 4: Lock compatibility baselines with failing/strict regression tests

**Files:**
- Modify: `tests/tools/coordinator/coordinator-tools-test.cpp`
- Modify: `tests/tools/swarm/swarm-tools-test.cpp`
- Modify: `tests/tools/task/task-tool-test.cpp`
- Modify: `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- Modify: `tests/tools/inbox/inbox-tool-test.cpp`
- Modify: `tests/tools/message-attachments/message-attachments-tool-test.cpp`

- [ ] **Step 1: Add strict assertions for unknown-op error text in `task/heartbeat/inbox`**

```cpp
CHECK(result.content == "Error: unknown operation. Supported: list, ack, clear.");
```

- [ ] **Step 2: Add strict assertions for missing `id/name` error paths**

```cpp
CHECK(result.content == "Error: id or name is required.");
CHECK(inbox_ack_without_id.content == "Error: id is required.");
```

- [ ] **Step 3: Add explicit A/B context strategy tests and message_attachments compatibility assertions**

```cpp
// A-class: message_attachments not registered when non-channel context.
CHECK(registry.find_definition("message_attachments") == nullptr);

// B-class: task registered in runtime but execute returns context error.
CHECK(result.content == "Error: task tool is not available in this context.");

// message_attachments contract snapshots
CHECK(unknown_op_payload.at("error").get<std::string>() == "unknown operation. Supported: list, download.");
CHECK(no_attachments_payload.at("error").get<std::string>() == "the current message has no attachments.");
CHECK(download_unavailable_payload.at("error").get<std::string>() == "attachment downloads are not available in this context.");

// coordinator/swarm key failure snapshots
CHECK(coordinator_missing_recipient_payload.at("error").get<std::string>() == "Either run_id or to must be specified");
CHECK(swarm_missing_manager_payload.at("error").get<std::string>() == "Team manager is not available");
```

- [ ] **Step 4: Run tool tests to confirm new assertions catch drift**

Run: `xmake run test-tools -- "[tools]"`
Expected: PASS on current behavior, otherwise FAIL and fix tests to match current baseline only (no speculative changes).

- [ ] **Step 5: Commit baseline tests**

```bash
git add tests/tools/coordinator/coordinator-tools-test.cpp tests/tools/swarm/swarm-tools-test.cpp tests/tools/task/task-tool-test.cpp tests/tools/heartbeat/heartbeat-tool-test.cpp tests/tools/inbox/inbox-tool-test.cpp tests/tools/message-attachments/message-attachments-tool-test.cpp
git commit -m "test(tools): lock context gating and error-text compatibility baselines"
```

### Task 5: Migrate coordinator/swarm registration to builder/group

**Files:**
- Modify: `src/tools/coordinator/agent-spawn-tool.cpp`
- Modify: `src/tools/coordinator/agent-send-message-tool.cpp`
- Modify: `src/tools/coordinator/agent-stop-tool.cpp`
- Modify: `src/tools/swarm/team-create-tool.cpp`
- Modify: `src/tools/swarm/team-delete-tool.cpp`
- Modify: `tests/tools/coordinator/coordinator-tools-test.cpp`
- Modify: `tests/tools/swarm/swarm-tools-test.cpp`

- [ ] **Step 1: Write a temporary failing regression for deferred flag visibility (if not already strict)**

```cpp
const auto *def = registry.find_definition("agent_spawn");
REQUIRE(def != nullptr);
CHECK(registry.has_deferred_tools());
```

- [ ] **Step 2: Run baseline coordinator/swarm tests before migration edits**

Run: `xmake run test-tools -- "[tools][coordinator],[tools][swarm]"`
Expected: PASS.

- [ ] **Step 3: If Step 2 fails due to newly added assertions, fix test setup and rerun to PASS**

Run: `xmake run test-tools -- "[tools][coordinator],[tools][swarm]"`
Expected: PASS after test-setup correction only (no production refactor yet).

- [ ] **Step 4: Replace manual `registry.register_tool({...})` blocks with `ToolSpecBuilder`**

```cpp
registry.register_tool(make_tool_spec_builder()
    .name("agent_spawn")
    .description("...")
    .input_schema(schema)
    .deferred(true)
    .execute([tool_context](const nlohmann::json &input) {
        return agent_spawn_handler(input, *tool_context);
    })
    .build());
```

- [ ] **Step 5: Keep function signatures and handlers unchanged**

Run: `xmake run test-tools -- "[tools][coordinator],[tools][swarm]"`
Expected: PASS with no output/contract drift.

- [ ] **Step 6: Commit coordinator/swarm migration**

```bash
git add src/tools/coordinator/agent-spawn-tool.cpp src/tools/coordinator/agent-send-message-tool.cpp src/tools/coordinator/agent-stop-tool.cpp src/tools/swarm/team-create-tool.cpp src/tools/swarm/team-delete-tool.cpp tests/tools/coordinator/coordinator-tools-test.cpp tests/tools/swarm/swarm-tools-test.cpp
git commit -m "refactor(tools): migrate coordinator and swarm registration to builder"
```

### Task 6: Migrate `task/heartbeat/inbox/message_attachments` to dispatch/schema helpers

**Files:**
- Modify: `src/tools/task/task-tool.cpp`
- Modify: `src/tools/heartbeat/heartbeat-tool.cpp`
- Modify: `src/tools/inbox/inbox-tool.cpp`
- Modify: `src/tools/message-attachments/message-attachments-tool.cpp`

- [ ] **Step 1: Ensure strict compatibility assertions are present, then confirm baseline PASS before code changes**

Run: `xmake run test-tools -- "[tools][task],[tools][heartbeat],[tools][inbox],[tools][message-attachments]"`
Expected: PASS before any migration edit; if gaps exist, add assertions and rerun until PASS.

- [ ] **Step 2: Migrate `task` to `dispatch_tool_op` + `schema-fragments` and run focused tests**

Run: `xmake run test-tools -- "[tools][task]"`
Expected: PASS with exact error text compatibility.

- [ ] **Step 3: Migrate `heartbeat` to `dispatch_tool_op` + `schema-fragments` and run focused tests**

Run: `xmake run test-tools -- "[tools][heartbeat]"`
Expected: PASS with exact error text compatibility.

- [ ] **Step 4: Migrate `inbox` to `dispatch_tool_op` + `schema-fragments` and run focused tests**

Run: `xmake run test-tools -- "[tools][inbox]"`
Expected: PASS with exact error text compatibility.

- [ ] **Step 5: Migrate `message_attachments` registration/schema path and run focused tests**

Run: `xmake run test-tools -- "[tools][message-attachments]"`
Expected: PASS with A-class gating semantics unchanged.

- [ ] **Step 6: Keep migration style equivalent to below pattern**

```cpp
return dispatch_tool_op(input,
    handler_map,
    [] { return std::string{"Error: unknown operation. Supported: add, update, remove, list, run."}; });
```

- [ ] **Step 7: Rebuild schema blocks from `schema-fragments` without changing fields**

```cpp
const auto input_schema = schema_fragments::object_with_required(...);
```

- [ ] **Step 8: Run combined tool-family regression after all four migrations**

Run: `xmake run test-tools -- "[tools][task],[tools][heartbeat],[tools][inbox],[tools][message-attachments]"`
Expected: PASS and exact compatibility strings unchanged.

- [ ] **Step 9: Commit automation-style migration**

```bash
git add src/tools/task/task-tool.cpp src/tools/heartbeat/heartbeat-tool.cpp src/tools/inbox/inbox-tool.cpp src/tools/message-attachments/message-attachments-tool.cpp
git commit -m "refactor(tools): unify automation-style tools with shared dispatch and schema"
```

---

## Chunk 3: Cleanup, Metrics, and Verification

### Task 7: Remove redundant helper duplication and consolidate includes

**Files:**
- Modify: `src/tools/coordinator/agent-spawn-tool.cpp`
- Modify: `src/tools/coordinator/agent-send-message-tool.cpp`
- Modify: `src/tools/coordinator/agent-stop-tool.cpp`
- Modify: `src/tools/swarm/team-create-tool.cpp`
- Modify: `src/tools/swarm/team-delete-tool.cpp`
- Modify: `src/tools/task/task-tool.cpp`
- Modify: `src/tools/heartbeat/heartbeat-tool.cpp`
- Modify: `src/tools/inbox/inbox-tool.cpp`
- Modify: `src/tools/message-attachments/message-attachments-tool.cpp`

- [ ] **Step 1: Edit the 9 migrated tool files to remove superseded local helpers and normalize includes**

Action:
- Delete local schema/op-dispatch helpers now replaced by `tool-dispatch` and `schema-fragments`.
- Keep only business handlers in each tool file.
- Keep include order aligned with project conventions.

- [ ] **Step 2: Build after cleanup edits**

Run: `xmake build -y`
Expected: Build succeeds.

- [ ] **Step 3: Verify quantitative migration metrics from spec**

Run: `rg -n "registry\.register_tool\(\{\.definition" src/tools/coordinator/agent-spawn-tool.cpp src/tools/coordinator/agent-send-message-tool.cpp src/tools/coordinator/agent-stop-tool.cpp src/tools/swarm/team-create-tool.cpp src/tools/swarm/team-delete-tool.cpp src/tools/task/task-tool.cpp src/tools/heartbeat/heartbeat-tool.cpp src/tools/inbox/inbox-tool.cpp src/tools/message-attachments/message-attachments-tool.cpp | wc -l`
Expected: `<= 5` (pre-migration baseline is 9; this satisfies the `>=35%` reduction target).

Run: `rg -n "make_tool_spec_builder\(" src/tools/coordinator/agent-spawn-tool.cpp src/tools/coordinator/agent-send-message-tool.cpp src/tools/coordinator/agent-stop-tool.cpp src/tools/swarm/team-create-tool.cpp src/tools/swarm/team-delete-tool.cpp src/tools/task/task-tool.cpp src/tools/heartbeat/heartbeat-tool.cpp src/tools/inbox/inbox-tool.cpp src/tools/message-attachments/message-attachments-tool.cpp | wc -l`
Expected: `>= 9` (all migrated tool registrations use shared builder path).

- [ ] **Step 4: Commit cleanup**

```bash
git add src/tools/coordinator/agent-spawn-tool.cpp src/tools/coordinator/agent-send-message-tool.cpp src/tools/coordinator/agent-stop-tool.cpp src/tools/swarm/team-create-tool.cpp src/tools/swarm/team-delete-tool.cpp src/tools/task/task-tool.cpp src/tools/heartbeat/heartbeat-tool.cpp src/tools/inbox/inbox-tool.cpp src/tools/message-attachments/message-attachments-tool.cpp
git commit -m "refactor(tools): remove duplicated registration and dispatch boilerplate"
```

### Task 8: Run regression suite for all affected tool paths

**Files:**
- Test: `tests/tools/registry/tool-abstraction-helpers-test.cpp`
- Test: `tests/tools/registry/tool-registry-test.cpp`
- Test: `tests/tools/coordinator/coordinator-tools-test.cpp`
- Test: `tests/tools/swarm/swarm-tools-test.cpp`
- Test: `tests/tools/task/task-tool-test.cpp`
- Test: `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- Test: `tests/tools/inbox/inbox-tool-test.cpp`
- Test: `tests/tools/message-attachments/message-attachments-tool-test.cpp`

- [ ] **Step 1: Run all tools tests**

Run: `xmake run test-tools`
Expected: PASS.

- [ ] **Step 2: Run focused registry compatibility subset**

Run: `xmake run test-tools -- "[tools][registry]"`
Expected: PASS.

- [ ] **Step 3: Check whether any test files changed during regression fixes**

Run: `git status --short tests/tools/ tests/coordinator/`
Expected: Either empty output (no test deltas) or only intentional compatibility-test edits.

- [ ] **Step 4: Commit test-fix deltas only when Step 3 shows changes**

```bash
if [ -n "$(git status --short tests/tools/ tests/coordinator/)" ]; then
  git add tests/tools/ tests/coordinator/
  git commit -m "test(tools): finalize compatibility and abstraction regression coverage"
fi
```

### Task 9: Final verification before merge/PR

**Files:**
- Verify only (no planned file edits)

- [ ] **Step 1: Run full test suite**

Run: `xmake test`
Expected: All test targets pass.

- [ ] **Step 2: Verify working tree is clean**

Run: `git status --short`
Expected: no output.

- [ ] **Step 3: Prepare concise changelog note for PR description**

```text
- unified tool registration via shared builder/group helpers
- migrated coordinator/swarm/automation-like tools without protocol changes
- added compatibility baselines for context gating and error text
```

---

## Definition of Done Checklist

- [ ] `tool-spec-builder/contextual-tool-group/tool-dispatch/schema-fragments` implemented with focused tests.
- [ ] 9 target tools migrated (`agent_spawn`, `agent_send_message`, `agent_stop`, `team_create`, `team_delete`, `task`, `heartbeat`, `inbox`, `message_attachments`).
- [ ] Tool names, input JSON fields, output/error compatibility unchanged.
- [ ] Deferred/discovery semantics unchanged (`definitions/find_definition/execute`).
- [ ] `xmake run test-tools` and `xmake test` both pass.
