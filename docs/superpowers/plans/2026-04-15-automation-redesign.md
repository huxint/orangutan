# Automation Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the legacy task, heartbeat, and inbox automation stack with one unified `Automation` model, one `AutomationService` boundary, one runtime scheduler, and one external `automation` protocol.

**Architecture:** Build the new automation module in focused layers: `model`, `builder`, `parser`, `repository`, `planner`, `delivery`, `service`, and `runtime`. Integrate that stack upward into tools, web routes, bootstrap/runtime bundles, and focused tests, then delete the old split APIs and storage model.

**Tech Stack:** C++23, `std::chrono`, `stdexec`, `sqlite3`, `nlohmann_json`, `magic_enum`, Catch2, xmake, CTest

---

## File Structure

Build note:

- `xmake/targets.lua` already globs `src/automation/*.cpp` and `src/tools/**/*.cpp`
- `xmake/tests.lua` already globs `tests/automation/*.cpp`, `tests/tools/**/*.cpp`, `tests/web/*.cpp`, and `tests/bootstrap/*.cpp`
- No build-file changes should be required unless the implementation introduces a brand-new top-level source directory

Target file map:

- Create: `src/automation/model.hpp`
- Create: `src/automation/model.cpp`
- Create: `src/automation/builder.hpp`
- Create: `src/automation/builder.cpp`
- Create: `src/automation/parser.hpp`
- Create: `src/automation/parser.cpp`
- Create: `src/automation/repository.hpp`
- Create: `src/automation/repository.cpp`
- Create: `src/automation/service.hpp`
- Create: `src/automation/service.cpp`
- Create: `src/automation/runtime.hpp`
- Create: `src/automation/runtime.cpp`
- Create: `src/automation/delivery.hpp`
- Create: `src/automation/delivery.cpp`
- Create: `src/tools/automation/automation-tool.hpp`
- Create: `src/tools/automation/automation-tool.cpp`
- Create: `tests/automation/automation-model-test.cpp`
- Create: `tests/automation/automation-planner-test.cpp`
- Create: `tests/automation/automation-repository-test.cpp`
- Create: `tests/automation/automation-service-runtime-test.cpp`
- Modify: `src/automation/cron-parser.hpp`
- Modify: `src/automation/cron-parser.cpp`
- Modify: `src/automation/planner.hpp`
- Modify: `src/automation/planner.cpp`
- Modify: `src/automation/log-writer.hpp`
- Modify: `src/automation/log-writer.cpp`
- Modify: `src/tools/automation/automation-tool-support.hpp`
- Modify: `src/tools/automation/automation-tool-support.cpp`
- Modify: `src/tools/register.hpp`
- Modify: `src/tools/register.cpp`
- Modify: `src/tools/registry/tool-context.hpp`
- Modify: `src/web/admin-routes.cpp`
- Modify: `src/web/web-routes.hpp`
- Modify: `src/web/web-server.hpp`
- Modify: `src/web/web-server.cpp`
- Modify: `src/web/web-types.hpp`
- Modify: `src/web/chat-routes.cpp`
- Modify: `src/web/web-routes.cpp`
- Modify: `src/bootstrap/app-runtime.hpp`
- Modify: `src/bootstrap/app-runtime.cpp`
- Modify: `src/bootstrap/bootstrap.cpp`
- Modify: `src/bootstrap/agent-runtime.hpp`
- Modify: `src/bootstrap/channel-serve.hpp`
- Modify: `src/bootstrap/channel-serve.cpp`
- Modify: `src/bootstrap/channel-serve-runtime.hpp`
- Modify: `src/bootstrap/runtime-assembler.hpp`
- Modify: `src/bootstrap/runtime-control.hpp`
- Modify: `src/bootstrap/runtime-control.cpp`
- Modify: `src/tools/background/background-completion.cpp`
- Modify: `tests/tools/registry/tool-registry-test.cpp`
- Modify: `tests/tools/shell/background-shell-completion-test.cpp`
- Modify: `tests/web/web-routes-test.cpp`
- Modify: `tests/web/web-chat-test.cpp`
- Modify: `tests/bootstrap/channel-serve-test.cpp`
- Modify: `tests/bootstrap/runtime-agent-runtime-test.cpp`
- Delete later: `src/automation/automation-types.hpp`
- Delete later: `src/automation/automation-types.cpp`
- Delete later: `src/automation/automation-store.hpp`
- Delete later: `src/automation/automation-store.cpp`
- Delete later: `src/automation/scheduler.hpp`
- Delete later: `src/automation/scheduler.cpp`
- Delete later: `src/tools/task/task-tool.hpp`
- Delete later: `src/tools/task/task-tool.cpp`
- Delete later: `src/tools/heartbeat/heartbeat-tool.hpp`
- Delete later: `src/tools/heartbeat/heartbeat-tool.cpp`
- Delete later: `src/tools/inbox/inbox-tool.hpp`
- Delete later: `src/tools/inbox/inbox-tool.cpp`
- Delete later: `tests/automation/automation-runtime-test.cpp`
- Delete later: `tests/automation/automation-store-test.cpp`
- Delete later: `tests/tools/task/task-tool-test.cpp`
- Delete later: `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- Delete later: `tests/tools/inbox/inbox-tool-test.cpp`

## Coverage Checklist

Every spec-required surface must appear in at least one failing test before implementation is considered done.

Service coverage:

- `save` and full-definition validation: Task 5, Step 1
- `find`, `list`, and `remove`: Task 5, Step 1
- `run_now`, `pause`, and `resume`: Task 5, Step 1
- `list_runs`, `list_deliveries`, `ack_delivery`, and `clear_deliveries`: Task 5, Step 1

Tool coverage:

- `create`, `update`, `remove`, `get`, and `list`: Task 6, Step 1
- `run`, `pause`, and `resume`: Task 6, Step 1
- `list_runs`, `list_deliveries`, `ack_delivery`, and `clear_deliveries`: Task 6, Step 1

Web coverage:

- `GET /automation`, `POST /automation`, `GET /automation/{id}`, `PATCH /automation/{id}`, and `DELETE /automation/{id}`: Task 7, Step 1
- `POST /automation/{id}/run`, `POST /automation/{id}/pause`, and `POST /automation/{id}/resume`: Task 7, Step 1
- `GET /automation/runs`, `GET /automation/deliveries`, `POST /automation/deliveries/{delivery_id}/ack`, and `DELETE /automation/deliveries`: Task 7, Step 1

## Task 1: Lay Down The Unified Model And Fluent Builder

**Files:**

- Create: `src/automation/model.hpp`
- Create: `src/automation/model.cpp`
- Create: `src/automation/builder.hpp`
- Create: `src/automation/builder.cpp`
- Create: `tests/automation/automation-model-test.cpp`
- Reference: `src/automation/automation-types.hpp`
- Reference: `docs/superpowers/specs/2026-04-15-automation-redesign-design.md`

- [ ] **Step 1: Write the failing model and builder tests**

```cpp
TEST_CASE("automation_builder_creates_cron_definition") {
    const auto automation = orangutan::automation::Automation::named("repo-check")
                                .for_agent("default")
                                .run_prompt("scan repo and summarize changes")
                                .cron("0 9 * * *")
                                .time_zone("Asia/Shanghai")
                                .deliver_to("owner")
                                .tag("daily")
                                .build();

    CHECK(automation.name == "repo-check");
    CHECK(automation.agent_key == "default");
    CHECK(automation.trigger.type == orangutan::automation::trigger_type::cron);
    CHECK(automation.delivery.mode == orangutan::automation::delivery_mode::notify);
}
```

- [ ] **Step 2: Run the automation test target and confirm the new types are missing**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: build or link failure mentioning missing `Automation`, builder methods, or trigger types.

- [ ] **Step 3: Add the minimal unified value types and fluent builder skeleton**

```cpp
enum class trigger_type : base::u8 {
    cron,
    interval,
    once,
};

struct Automation {
    std::string id;
    std::string agent_key;
    std::string name;
    Trigger trigger;
    DeliveryPolicy delivery;

    [[nodiscard]]
    static AutomationBuilder named(std::string_view name);
};
```

- [ ] **Step 4: Fill in builder validation until the new model tests pass**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: `test-automation` passes the new builder/model cases.

- [ ] **Step 5: Commit the model layer**

```bash
git add src/automation/model.* src/automation/builder.* tests/automation/automation-model-test.cpp
git commit -m "refactor: add unified automation model and builder"
```

## Task 2: Normalize Trigger Parsing And Wire Formats

**Files:**

- Create: `src/automation/parser.hpp`
- Create: `src/automation/parser.cpp`
- Modify: `src/automation/cron-parser.hpp`
- Modify: `src/automation/cron-parser.cpp`
- Modify: `tests/automation/automation-model-test.cpp`
- Create: `tests/automation/automation-planner-test.cpp`
- Reference: `docs/superpowers/specs/2026-04-15-automation-redesign-design.md`

- [ ] **Step 1: Add failing tests for canonical trigger wire formats**

```cpp
TEST_CASE("interval_trigger_json_uses_duration_strings_and_time_zone") {
    const auto automation = orangutan::automation::Automation::named("pulse")
                                .for_agent("default")
                                .run_prompt("status check")
                                .every(15min)
                                .jitter(30s)
                                .time_zone("UTC")
                                .within_hours({{"09:00", "18:00"}})
                                .deliver_silently()
                                .build();

    const auto json = orangutan::automation::trigger_to_json(automation.trigger);
    CHECK(json.at("type") == "interval");
    CHECK(json.at("every") == "15m");
    CHECK(json.at("jitter") == "30s");
}
```

- [ ] **Step 2: Run the automation tests and verify the JSON helpers fail first**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: failing assertions or missing-symbol errors for `trigger_to_json`, `trigger_from_json`, or duration parsing helpers.

- [ ] **Step 3: Implement parser helpers and reuse the existing cron parser behind the new API**

```cpp
[[nodiscard]]
nlohmann::json trigger_to_json(const Trigger &trigger);

[[nodiscard]]
std::expected<Trigger, std::string> trigger_from_json(const nlohmann::json &value);

[[nodiscard]]
std::expected<std::chrono::seconds, std::string> parse_duration_string(std::string_view value);
```

- [ ] **Step 4: Extend tests for `once`, `cron`, time-zone defaults, and active-window formatting**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: parser and model suites pass with concrete wire-format coverage.

- [ ] **Step 5: Commit the parsing layer**

```bash
git add src/automation/parser.* src/automation/cron-parser.* tests/automation/automation-model-test.cpp tests/automation/automation-planner-test.cpp
git commit -m "refactor: normalize automation trigger parsing"
```

## Task 3: Build The First-Generation Repository And Schema

**Files:**

- Create: `src/automation/repository.hpp`
- Create: `src/automation/repository.cpp`
- Create: `tests/automation/automation-repository-test.cpp`
- Reference: `src/storage/sqlite.hpp`
- Reference: `src/automation/automation-store.hpp`

- [ ] **Step 1: Write failing repository tests for save, lookup, uniqueness, runs, and deliveries**

```cpp
TEST_CASE("repository_enforces_agent_scoped_unique_names") {
    orangutan::automation::Repository repository(test_db_path);
    const auto automation = make_cron_automation("default", "repo-check");

    static_cast<void>(repository.save(automation));
    CHECK_THROWS(repository.save(automation));
}
```

- [ ] **Step 2: Run the automation test target and confirm the repository is missing**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: compile failure for missing `Repository`, schema methods, or delivery query helpers.

- [ ] **Step 3: Implement the `automations`, `automation_runs`, and `automation_deliveries` schema plus repository queries**

```cpp
class Repository {
public:
    explicit Repository(const std::filesystem::path &db_path);

    [[nodiscard]] std::string save(const Automation &automation);
    [[nodiscard]] std::optional<Automation> find(std::string_view agent_key, std::string_view id_or_name) const;
    [[nodiscard]] std::vector<DeliveryRecord> list_deliveries(const DeliveryQuery &query) const;
    void clear_deliveries(const DeliveryQuery &query);
};
```

- [ ] **Step 4: Add tests for `clear_deliveries(DeliveryQuery)` bulk-ack behavior and one-shot persistence markers**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: repository tests pass and prove agent-scoped uniqueness plus bulk-ack semantics.

- [ ] **Step 5: Commit the repository layer**

```bash
git add src/automation/repository.* tests/automation/automation-repository-test.cpp
git commit -m "refactor: add unified automation repository"
```

## Task 4: Rebuild Planner Semantics Around Cron, Interval, And Once

**Files:**

- Modify: `src/automation/planner.hpp`
- Modify: `src/automation/planner.cpp`
- Modify: `tests/automation/automation-planner-test.cpp`
- Reference: `src/automation/model.hpp`
- Reference: `src/automation/parser.hpp`

- [ ] **Step 1: Add failing planner tests for due detection, fixed-delay interval cadence, and one-shot consumption**

```cpp
TEST_CASE("interval_uses_fixed_delay_from_completion_time") {
    auto automation = make_interval_automation();
    automation.state.last_run_at = 100;
    automation.state.next_due_at = 130;

    const auto completion = orangutan::automation::from_unix_seconds(140);
    const auto next_due = orangutan::automation::plan_next_due(automation, completion);
    REQUIRE(next_due.has_value());
    CHECK(*next_due == 170);
}
```

- [ ] **Step 2: Run planner tests and confirm the old planner API no longer fits**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: failing planner assertions or compile errors around legacy `TaskSpec` and `HeartbeatSpec`.

- [ ] **Step 3: Replace the planner API with unified due-item calculations**

```cpp
struct DueAutomation {
    Automation automation;
    base::i64 scheduled_for = 0;
};

[[nodiscard]]
std::vector<DueAutomation> collect_due_automations(std::span<const Automation> automations, TimePoint now);
```

- [ ] **Step 4: Add tests for active windows, cron time zones, startup behavior, and no catch-up interval backfill**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: planner suite passes with explicit fixed-delay and one-shot behavior coverage.

- [ ] **Step 5: Commit the planner rewrite**

```bash
git add src/automation/planner.* tests/automation/automation-planner-test.cpp
git commit -m "refactor: rebuild automation planner semantics"
```

## Task 5: Add Delivery Handling, Service APIs, And Runtime Orchestration

**Files:**

- Create: `src/automation/delivery.hpp`
- Create: `src/automation/delivery.cpp`
- Create: `src/automation/service.hpp`
- Create: `src/automation/service.cpp`
- Create: `src/automation/runtime.hpp`
- Create: `src/automation/runtime.cpp`
- Modify: `src/automation/log-writer.hpp`
- Modify: `src/automation/log-writer.cpp`
- Create: `tests/automation/automation-service-runtime-test.cpp`
- Reference: `src/automation/repository.hpp`
- Reference: `src/automation/planner.hpp`

- [ ] **Step 1: Write failing service/runtime tests for the full service surface**

```cpp
TEST_CASE("service_run_now_executes_without_changing_disabled_state") {
    auto harness = make_service_harness();
    const auto id = harness.save(make_disabled_once_automation());

    const auto run_id = harness.service.run_now("default", id);
    CHECK_FALSE(harness.service.find("default", id)->enabled);
    CHECK_FALSE(run_id.empty());
}
```

Add companion cases for:

- `find`, `list`, and `remove`
- `pause` and `resume`
- `list_runs`
- `ack_delivery` and `clear_deliveries(const DeliveryQuery &query)`
- per-agent lease serialization
- silent versus notify delivery behavior

- [ ] **Step 2: Run the automation target and confirm service/runtime symbols are missing**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: compile failure for `AutomationService`, delivery record handling, or runtime lease orchestration.

- [ ] **Step 3: Implement the service and runtime boundaries with `stdexec`-based execution flow**

```cpp
class AutomationService {
public:
    [[nodiscard]] std::string save(Automation automation);
    [[nodiscard]] std::vector<Automation> list(const AutomationQuery &query) const;
    [[nodiscard]] std::optional<Automation> find(std::string_view agent_key, std::string_view id_or_name) const;
    [[nodiscard]] bool remove(std::string_view agent_key, std::string_view id_or_name);
    [[nodiscard]] std::string run_now(std::string_view agent_key, std::string_view id_or_name);
    [[nodiscard]] bool pause(std::string_view agent_key, std::string_view id_or_name);
    [[nodiscard]] bool resume(std::string_view agent_key, std::string_view id_or_name);
    [[nodiscard]] std::vector<RunRecord> list_runs(const RunQuery &query) const;
    [[nodiscard]] std::vector<DeliveryRecord> list_deliveries(const DeliveryQuery &query) const;
    [[nodiscard]] bool ack_delivery(std::string_view agent_key, std::string_view delivery_id);
    void clear_deliveries(const DeliveryQuery &query);
};
```

- [ ] **Step 4: Make the service/runtime tests pass, including `notify` versus `silent` delivery behavior**

Run: `xmake build test-automation`

Run: `ctest --test-dir build -R test-automation --output-on-failure`

Expected: service/runtime suite passes with unified automation semantics.

- [ ] **Step 5: Commit the operational core**

```bash
git add src/automation/delivery.* src/automation/service.* src/automation/runtime.* src/automation/log-writer.* tests/automation/automation-service-runtime-test.cpp
git commit -m "refactor: add automation service and runtime"
```

## Task 6: Replace The Split Tools With One Unified Automation Tool

**Files:**

- Create: `src/tools/automation/automation-tool.hpp`
- Create: `src/tools/automation/automation-tool.cpp`
- Modify: `src/tools/automation/automation-tool-support.hpp`
- Modify: `src/tools/automation/automation-tool-support.cpp`
- Modify: `src/tools/register.hpp`
- Modify: `src/tools/register.cpp`
- Modify: `src/tools/registry/tool-context.hpp`
- Modify: `tests/tools/registry/tool-registry-test.cpp`
- Create or modify: `tests/tools/automation/automation-tool-test.cpp`
- Delete later: `src/tools/task/task-tool.hpp`
- Delete later: `src/tools/task/task-tool.cpp`
- Delete later: `src/tools/heartbeat/heartbeat-tool.hpp`
- Delete later: `src/tools/heartbeat/heartbeat-tool.cpp`
- Delete later: `src/tools/inbox/inbox-tool.hpp`
- Delete later: `src/tools/inbox/inbox-tool.cpp`

- [ ] **Step 1: Add failing unified-tool tests for the full protocol surface**

```cpp
TEST_CASE("automation_tool_updates_delivery_by_replacement") {
    auto harness = make_automation_tool_harness();
    static_cast<void>(invoke_tool(harness, R"({"op":"create","name":"repo-check","prompt":"scan","trigger":{"type":"cron","cron":"0 9 * * *"},"delivery":{"mode":"notify","targets":["owner"]}})"));

    const auto response = invoke_tool(harness, R"({"op":"update","name":"repo-check","delivery":{"mode":"silent","targets":[]}})");
    CHECK(response.find("Updated automation") != std::string::npos);
}
```

Add companion tool cases for:

- `get`, `list`, and `remove`
- `run`, `pause`, and `resume`
- `list_runs`
- `list_deliveries`, `ack_delivery`, and `clear_deliveries`
- id-based and name-based selector resolution

- [ ] **Step 2: Run tool tests and confirm the old three-tool registration still blocks the new contract**

Run: `xmake build test-tools`

Run: `ctest --test-dir build -R test-tools --output-on-failure`

Expected: missing registration or wrong-schema failures around the new `automation` tool.

- [ ] **Step 3: Implement the unified tool and switch `ToolRuntimeContext` to the new service boundary**

```cpp
void register_automation_tool(ToolRegistry &registry, const ToolRuntimeContext *tool_context);
```

- [ ] **Step 4: Remove task, heartbeat, and inbox registration paths after the new tool tests pass**

Run: `xmake build test-tools`

Run: `ctest --test-dir build -R test-tools --output-on-failure`

Expected: tool tests pass and no built-in registration still exposes `task`, `heartbeat`, or `inbox`.

- [ ] **Step 5: Commit the tool-surface replacement**

```bash
git add src/tools/automation/automation-tool.* src/tools/automation/automation-tool-support.* src/tools/register.* src/tools/registry/tool-context.hpp tests/tools/automation/automation-tool-test.cpp tests/tools/registry/tool-registry-test.cpp
git commit -m "refactor: unify automation tool surface"
```

## Task 7: Replace Web And Bootstrap Integration With Unified Automation Semantics

**Files:**

- Modify: `src/web/admin-routes.cpp`
- Modify: `src/web/web-routes.hpp`
- Modify: `src/web/web-server.hpp`
- Modify: `src/web/web-server.cpp`
- Modify: `src/web/web-types.hpp`
- Modify: `src/web/chat-routes.cpp`
- Modify: `src/web/web-routes.cpp`
- Modify: `src/bootstrap/app-runtime.hpp`
- Modify: `src/bootstrap/app-runtime.cpp`
- Modify: `src/bootstrap/bootstrap.cpp`
- Modify: `src/bootstrap/agent-runtime.hpp`
- Modify: `src/bootstrap/channel-serve.hpp`
- Modify: `src/bootstrap/channel-serve.cpp`
- Modify: `src/bootstrap/channel-serve-runtime.hpp`
- Modify: `src/bootstrap/runtime-assembler.hpp`
- Modify: `src/bootstrap/runtime-control.hpp`
- Modify: `src/bootstrap/runtime-control.cpp`
- Modify: `src/tools/background/background-completion.cpp`
- Modify: `tests/web/web-routes-test.cpp`
- Modify: `tests/web/web-chat-test.cpp`
- Modify: `tests/bootstrap/channel-serve-test.cpp`
- Modify: `tests/bootstrap/runtime-agent-runtime-test.cpp`
- Modify: `tests/tools/shell/background-shell-completion-test.cpp`

- [ ] **Step 1: Add failing route and runtime-bundle tests for the full HTTP surface**

```cpp
TEST_CASE("web_server_lists_automations_from_unified_service") {
    auto harness = make_web_automation_harness();
    const auto response = harness.get("/automation?agent_key=default");
    CHECK(response.status == 200);
    CHECK(response.body.find("repo-check") != std::string::npos);
}
```

Add companion route cases for:

- `POST /automation`
- `GET /automation/{id}`
- `PATCH /automation/{id}`
- `DELETE /automation/{id}`
- `POST /automation/{id}/run`
- `POST /automation/{id}/pause`
- `POST /automation/{id}/resume`
- `GET /automation/runs`
- `GET /automation/deliveries`
- `POST /automation/deliveries/{delivery_id}/ack`
- `DELETE /automation/deliveries`

- [ ] **Step 2: Run web, tool, and bootstrap tests and verify the old task/heartbeat/inbox handlers fail**

Run: `xmake build test-tools test-web test-bootstrap`

Run: `ctest --test-dir build -R "test-(tools|web|bootstrap)" --output-on-failure`

Expected: route or bundle failures tied to missing unified automation handlers.

- [ ] **Step 3: Replace runtime bundle wiring, server setters, and admin/web routes with the new service API**

```cpp
class WebServer {
public:
    void set_automation_service(automation::AutomationService *service);
};
```

- [ ] **Step 4: Update chat, background completion, and channel-serve paths to write delivery records instead of inbox items**

Run: `xmake build test-tools test-web test-bootstrap`

Run: `ctest --test-dir build -R "test-(tools|web|bootstrap)" --output-on-failure`

Expected: web, bootstrap, and shell-background tests pass with unified automation routes and delivery wiring.

- [ ] **Step 5: Commit the integration rewrite**

```bash
git add src/web/*.cpp src/web/*.hpp src/bootstrap/app-runtime.* src/bootstrap/runtime-control.cpp tests/web/*.cpp tests/bootstrap/*.cpp
git commit -m "refactor: integrate unified automation service into web and bootstrap"
```

## Task 8: Delete Legacy Automation Paths And Finish Regression Coverage

**Files:**

- Delete: `src/automation/automation-types.hpp`
- Delete: `src/automation/automation-types.cpp`
- Delete: `src/automation/automation-store.hpp`
- Delete: `src/automation/automation-store.cpp`
- Delete: `src/automation/scheduler.hpp`
- Delete: `src/automation/scheduler.cpp`
- Delete: `src/tools/task/task-tool.hpp`
- Delete: `src/tools/task/task-tool.cpp`
- Delete: `src/tools/heartbeat/heartbeat-tool.hpp`
- Delete: `src/tools/heartbeat/heartbeat-tool.cpp`
- Delete: `src/tools/inbox/inbox-tool.hpp`
- Delete: `src/tools/inbox/inbox-tool.cpp`
- Delete: `tests/automation/automation-runtime-test.cpp`
- Delete: `tests/automation/automation-store-test.cpp`
- Delete: `tests/tools/task/task-tool-test.cpp`
- Delete: `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- Delete: `tests/tools/inbox/inbox-tool-test.cpp`
- Verify: `tests/automation/automation-model-test.cpp`
- Verify: `tests/automation/automation-planner-test.cpp`
- Verify: `tests/automation/automation-repository-test.cpp`
- Verify: `tests/automation/automation-service-runtime-test.cpp`
- Verify: `tests/tools/shell/background-shell-completion-test.cpp`

- [ ] **Step 1: Remove the old split-source and test files once all call sites are on the new stack**

```bash
git rm src/automation/automation-types.hpp src/automation/automation-types.cpp
git rm src/automation/automation-store.hpp src/automation/automation-store.cpp
git rm src/automation/scheduler.hpp src/automation/scheduler.cpp
git rm src/tools/task/task-tool.hpp src/tools/task/task-tool.cpp
git rm src/tools/heartbeat/heartbeat-tool.hpp src/tools/heartbeat/heartbeat-tool.cpp
git rm src/tools/inbox/inbox-tool.hpp src/tools/inbox/inbox-tool.cpp
git rm tests/automation/automation-runtime-test.cpp tests/automation/automation-store-test.cpp
git rm tests/tools/task/task-tool-test.cpp tests/tools/heartbeat/heartbeat-tool-test.cpp tests/tools/inbox/inbox-tool-test.cpp
```

- [ ] **Step 2: Run focused regression targets for automation, tools, web, and bootstrap**

Run: `xmake build test-automation test-tools test-web test-bootstrap`

Run: `ctest --test-dir build -R "test-(automation|tools|web|bootstrap)" --output-on-failure`

Expected: all focused targets pass without references to legacy task, heartbeat, inbox, store, or scheduler APIs.

- [ ] **Step 3: Grep for legacy automation call sites before the final smoke pass**

Run: `rg -n "automation::Store|automation::Runtime|save_task|save_heartbeat|run_task_now|run_heartbeat_now|list_tasks\\(|list_heartbeats\\(|list_inbox\\(|ack_inbox|clear_inbox|register_task_tool|register_heartbeat_tool|register_inbox_tool" src tests`

Expected: no matches in `src/` or `tests/` outside intentionally deleted paths or historical docs.

- [ ] **Step 4: Run one broad smoke pass over the library and remaining tool routes**

Run: `xmake build`

Run: `ctest --test-dir build -R "test-(automation|tools|web|bootstrap|cli)" --output-on-failure`

Expected: build is green and no remaining compile references to deleted automation files exist.

- [ ] **Step 5: Update the design and plan docs only if implementation diverged from the approved spec**

```markdown
- if code matched the spec, leave docs unchanged
- if implementation needed a justified deviation, document it before the final commit
```

- [ ] **Step 6: Commit the cleanup and regression pass**

```bash
git add -A
git commit -m "refactor: remove legacy automation stack"
```
