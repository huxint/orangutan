# Automation Core Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Orangutan's current polling-based automation subsystem with an embeddable automation core, indexed persistent schedule state, a thin optional runtime driver, and a fluent modern C++ DSL while preserving existing user-visible functionality.

**Architecture:** Introduce a thread-free kernel over explicit `JobDefinition` and `ScheduleState` records, backed by a SQLite store optimized for `next_due_at` and due-job reservation. Layer a small runtime driver and DSL above the kernel, then bridge the existing service/runtime callers onto the new stack before removing the legacy polling model.

**Tech Stack:** C++23, `std::expected`, `std::variant`, SQLite expected API, stdexec/task-pool runtime, Catch2, xmake

---

## File Structure

### New files

- `src/automation/core-model.hpp`
  - New value objects: `JobId`, `ExecutionId`, `ScheduleSpec`, `JobDefinition`, `ScheduleState`, `DispatchRequest`, `ExecutionPolicy`, `ResultPolicy`
- `src/automation/core-model.cpp`
  - ID helpers, serialization helpers, small value-object utilities
- `src/automation/action-registry.hpp`
  - `ActionDescriptor`, payload codec hooks, action registration entry points
- `src/automation/store.hpp`
  - Thread-free persistence interface used by the kernel
- `src/automation/sqlite-store.hpp`
  - SQLite-backed store declaration
- `src/automation/sqlite-store.cpp`
  - Schema, due-query SQL, reservation SQL, execution persistence
- `src/automation/kernel.hpp`
  - Core scheduling API
- `src/automation/kernel.cpp`
  - Schedule-state transitions, reservation logic, recovery logic
- `src/automation/driver.hpp`
  - Optional runtime driver declaration
- `src/automation/driver.cpp`
  - Sleep-until-next-due loop, explicit wakeup, dispatch bridge
- `src/automation/dsl.hpp`
  - Fluent builder API for job definitions and chained action descriptors
- `src/automation/dsl.cpp`
  - Builder validation and codec glue
- `tests/automation/automation-core-model-test.cpp`
  - Value object and serialization tests
- `tests/automation/automation-store-test.cpp`
  - Store and SQL behavior tests
- `tests/automation/automation-kernel-test.cpp`
  - Reservation, policy, and recovery tests
- `tests/automation/automation-driver-test.cpp`
  - Idle sleep, wakeup, and dispatch tests
- `tests/automation/automation-dsl-test.cpp`
  - Fluent builder and typed payload tests

### Existing files to modify

- `src/automation/repository.hpp`
  - Compatibility facade or transitional wrapper during migration
- `src/automation/repository.cpp`
  - Transitional bridge or removal once sqlite store is adopted
- `src/automation/service.hpp`
  - Route legacy callers through kernel/store-compatible APIs
- `src/automation/service.cpp`
  - Replace `collect_due` and mixed persistence logic with kernel/store calls
- `src/automation/runtime.hpp`
  - Convert legacy runtime to thin compatibility wrapper over the new driver
- `src/automation/runtime.cpp`
  - Remove polling scheduler responsibilities
- `src/bootstrap/app-runtime.hpp`
  - Own driver lifecycle instead of legacy runtime loop assumptions
- `src/bootstrap/app-runtime.cpp`
  - Instantiate new driver and wiring
- `src/bootstrap/bootstrap.cpp`
  - Start automation driver lazily or conditionally
- `src/bootstrap/channel-serve.cpp`
  - Continue using automation APIs through the new compatibility layer
- `tests/automation/automation-service-runtime-test.cpp`
  - Rebase old integration expectations onto the new driver/kernel architecture
- `tests/automation/automation-repository-test.cpp`
  - Update or reduce legacy repository-only assertions during migration

### Existing files likely referenced but not heavily changed

- `src/automation/planner.hpp`
- `src/automation/planner.cpp`
- `src/automation/model.hpp`
- `src/automation/model.cpp`
- `src/automation/builder.hpp`
- `src/automation/builder.cpp`
- `src/automation/delivery.hpp`
- `src/automation/delivery.cpp`
- `src/bootstrap/runtime-control.cpp`
- `src/bootstrap/agent-runtime.cpp`

Keep these stable unless a task explicitly needs them.

---

### Task 1: Introduce Core Model Types

**Files:**
- Create: `src/automation/core-model.hpp`
- Create: `src/automation/core-model.cpp`
- Test: `tests/automation/automation-core-model-test.cpp`

- [ ] **Step 1: Write failing model tests**

Add tests for:
- schedule variant round-trips
- strong typed IDs
- `ExecutionPolicy` defaults
- `ScheduleState` default values

```cpp
TEST_CASE("core model uses schedule variants") {
    using orangutan::automation::CronSchedule;
    using orangutan::automation::ScheduleSpec;

    const ScheduleSpec spec = CronSchedule{
        .expr = "0 * * * *",
        .time_zone = "UTC",
    };

    CHECK(std::holds_alternative<CronSchedule>(spec));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `xmake run test-automation "core model uses schedule variants"`
Expected: FAIL because `core-model.hpp` and types do not exist yet

- [ ] **Step 3: Write minimal implementation**

Implement the new value objects in `core-model.hpp/.cpp` using:
- `std::variant` for `ScheduleSpec`
- strong typed IDs
- explicit policy types
- plain value semantics

- [ ] **Step 4: Run focused tests**

Run: `xmake run test-automation "[automation][core-model]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/automation/automation-core-model-test.cpp src/automation/core-model.hpp src/automation/core-model.cpp
git commit -m "feat: add automation core model types"
```

### Task 2: Add Store Interface and SQLite Schema

**Files:**
- Create: `src/automation/store.hpp`
- Create: `src/automation/sqlite-store.hpp`
- Create: `src/automation/sqlite-store.cpp`
- Test: `tests/automation/automation-store-test.cpp`

- [ ] **Step 1: Write failing store tests**

Cover:
- saving `JobDefinition + ScheduleState`
- `next_due_at()` query
- `list_due(now, limit)`
- job removal

```cpp
TEST_CASE("sqlite store returns earliest next_due_at") {
    // save two jobs with different next_due_at values
    // expect the earliest one back
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `xmake run test-automation "sqlite store returns earliest next_due_at"`
Expected: FAIL because the store interface and schema do not exist

- [ ] **Step 3: Write minimal implementation**

Implement:
- `StoreError`
- `JobStore` interface
- SQLite schema for `automation_job_definitions`, `automation_job_state`, `automation_executions`, `automation_deliveries`
- expected-based SQL helpers only

- [ ] **Step 4: Run focused tests**

Run: `xmake run test-automation "[automation][store]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/automation/automation-store-test.cpp src/automation/store.hpp src/automation/sqlite-store.hpp src/automation/sqlite-store.cpp
git commit -m "feat: add automation sqlite store"
```

### Task 3: Implement Due Reservation and Lease Semantics

**Files:**
- Modify: `src/automation/store.hpp`
- Modify: `src/automation/sqlite-store.hpp`
- Modify: `src/automation/sqlite-store.cpp`
- Test: `tests/automation/automation-store-test.cpp`

- [ ] **Step 1: Add failing reservation tests**

Cover:
- reserve only due jobs
- respect limit
- write `lease_owner`
- skip already leased jobs
- reclaim expired leases

```cpp
TEST_CASE("sqlite store reserves due jobs with lease metadata") {
    // save due jobs
    // reserve from driver "driver-a"
    // expect lease_owner and lease_expires_at to be set
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `xmake run test-automation "sqlite store reserves due jobs with lease metadata"`
Expected: FAIL because reservation behavior is not implemented

- [ ] **Step 3: Write minimal implementation**

Add atomic reservation helpers and supporting SQL. Use revision bumps on state updates.

- [ ] **Step 4: Run focused tests**

Run: `xmake run test-automation "[automation][store][lease]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/automation/automation-store-test.cpp src/automation/store.hpp src/automation/sqlite-store.hpp src/automation/sqlite-store.cpp
git commit -m "feat: add automation due-job reservation"
```

### Task 4: Implement the Thread-Free Kernel

**Files:**
- Create: `src/automation/kernel.hpp`
- Create: `src/automation/kernel.cpp`
- Test: `tests/automation/automation-kernel-test.cpp`

- [ ] **Step 1: Write failing kernel tests**

Cover:
- `next_wakeup()`
- `reserve_due()`
- `mark_started()`
- `mark_finished()`
- `recover()`

```cpp
TEST_CASE("kernel emits one recovery dispatch for run_once missed policy") {
    // create job with missed schedule points
    // reserve due
    // expect exactly one DispatchRequest
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `xmake run test-automation "kernel emits one recovery dispatch for run_once missed policy"`
Expected: FAIL because the kernel does not exist

- [ ] **Step 3: Write minimal implementation**

Implement kernel logic over the store:
- map due jobs to `DispatchRequest`
- apply `skip`, `run_once`, and `catch_up`
- update schedule state through explicit transitions

- [ ] **Step 4: Run focused tests**

Run: `xmake run test-automation "[automation][kernel]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/automation/automation-kernel-test.cpp src/automation/kernel.hpp src/automation/kernel.cpp
git commit -m "feat: add automation scheduling kernel"
```

### Task 5: Add Action Registry and Execution Ports

**Files:**
- Create: `src/automation/action-registry.hpp`
- Modify: `src/automation/core-model.hpp`
- Test: `tests/automation/automation-core-model-test.cpp`

- [ ] **Step 1: Write failing registry tests**

Cover:
- registering an action key
- encoding a typed payload
- rejecting duplicate action keys

```cpp
TEST_CASE("action registry rejects duplicate action keys") {
    // register same key twice and expect error
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `xmake run test-automation "action registry rejects duplicate action keys"`
Expected: FAIL because the action registry does not exist

- [ ] **Step 3: Write minimal implementation**

Define:
- `ActionDescriptor`
- payload codec hooks
- `ActionRegistry`
- `ExecutionContext`
- `ExecutorPort`

- [ ] **Step 4: Run focused tests**

Run: `xmake run test-automation "[automation][action]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/automation/automation-core-model-test.cpp src/automation/core-model.hpp src/automation/action-registry.hpp
git commit -m "feat: add automation action registry"
```

### Task 6: Implement the Thin Runtime Driver

**Files:**
- Create: `src/automation/driver.hpp`
- Create: `src/automation/driver.cpp`
- Test: `tests/automation/automation-driver-test.cpp`

- [ ] **Step 1: Write failing driver tests**

Cover:
- idle driver sleeps without periodic polling
- mutation wakeup breaks sleep
- due jobs dispatch through executor
- stop shuts the driver down cleanly

```cpp
TEST_CASE("driver stays idle when there are no due jobs") {
    // start driver on empty store
    // wait briefly
    // assert no dispatches occurred
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `xmake run test-automation "driver stays idle when there are no due jobs"`
Expected: FAIL because the driver does not exist

- [ ] **Step 3: Write minimal implementation**

Implement:
- `sleep_until(next_due_at)`
- explicit wakeup signal on mutation
- dispatch through `ExecutorPort`
- stdexec/task-pool integration only in the driver layer

- [ ] **Step 4: Run focused tests**

Run: `xmake run test-automation "[automation][driver]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/automation/automation-driver-test.cpp src/automation/driver.hpp src/automation/driver.cpp
git commit -m "feat: add automation runtime driver"
```

### Task 7: Add Fluent DSL and Typed Binding Support

**Files:**
- Create: `src/automation/dsl.hpp`
- Create: `src/automation/dsl.cpp`
- Test: `tests/automation/automation-dsl-test.cpp`

- [ ] **Step 1: Write failing DSL tests**

Cover:
- building a cron job
- adding retry and delivery policy
- binding a typed payload
- building a chained pipeline descriptor

```cpp
TEST_CASE("dsl builds cron job definition with typed payload") {
    // build a job with automation::job(...)
    // assert action_key, schedule, and policies are populated
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `xmake run test-automation "dsl builds cron job definition with typed payload"`
Expected: FAIL because the DSL does not exist

- [ ] **Step 3: Write minimal implementation**

Implement the fluent builders as syntax sugar over `JobDefinition` and `ActionDescriptor`.

- [ ] **Step 4: Run focused tests**

Run: `xmake run test-automation "[automation][dsl]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/automation/automation-dsl-test.cpp src/automation/dsl.hpp src/automation/dsl.cpp
git commit -m "feat: add automation fluent dsl"
```

### Task 8: Bridge Legacy Service and Runtime onto the New Core

**Files:**
- Modify: `src/automation/service.hpp`
- Modify: `src/automation/service.cpp`
- Modify: `src/automation/runtime.hpp`
- Modify: `src/automation/runtime.cpp`
- Modify: `tests/automation/automation-service-runtime-test.cpp`

- [ ] **Step 1: Add failing compatibility tests**

Extend the existing integration tests to assert that legacy entry points still:
- save jobs
- start automation execution
- record runs and deliveries
- restart correctly

- [ ] **Step 2: Run test to verify it fails**

Run: `xmake run test-automation "[automation][service][runtime]"`
Expected: FAIL because compatibility routing is not implemented

- [ ] **Step 3: Write minimal implementation**

Make the old service/runtime surface a compatibility facade over:
- `JobStore`
- `Kernel`
- `Driver`

Preserve current user-visible behavior while deleting old mixed scheduling logic.

- [ ] **Step 4: Run focused tests**

Run: `xmake run test-automation "[automation][service][runtime]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/automation/automation-service-runtime-test.cpp src/automation/service.hpp src/automation/service.cpp src/automation/runtime.hpp src/automation/runtime.cpp
git commit -m "refactor: route automation service through kernel and driver"
```

### Task 9: Rewire Bootstrap and App Runtime

**Files:**
- Modify: `src/bootstrap/app-runtime.hpp`
- Modify: `src/bootstrap/app-runtime.cpp`
- Modify: `src/bootstrap/bootstrap.cpp`
- Modify: `src/bootstrap/channel-serve.cpp`

- [ ] **Step 1: Add failing integration checks**

Use or extend existing automation startup tests to cover:
- no hot polling when idle
- driver starts with app runtime
- channel mode continues to use automation APIs correctly

- [ ] **Step 2: Run test/build to verify it fails**

Run: `xmake build test-automation`
Expected: FAIL because bootstrap still expects the old runtime model

- [ ] **Step 3: Write minimal implementation**

Wire the driver into `AppRuntime`, remove direct dependence on the legacy polling runtime, and keep existing bootstrap behavior intact.

- [ ] **Step 4: Run focused verification**

Run: `xmake run test-automation`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/bootstrap/app-runtime.hpp src/bootstrap/app-runtime.cpp src/bootstrap/bootstrap.cpp src/bootstrap/channel-serve.cpp
git commit -m "refactor: wire bootstrap to automation driver"
```

### Task 10: Remove Legacy Polling Model and Finalize Cleanup

**Files:**
- Modify: `src/automation/repository.hpp`
- Modify: `src/automation/repository.cpp`
- Modify: `tests/automation/automation-repository-test.cpp`
- Modify: `tests/automation/automation-service-runtime-test.cpp`
- Modify: `src/automation/model.hpp`
- Modify: `src/automation/model.cpp`

- [ ] **Step 1: Add failing cleanup assertions**

Add regression tests that prove:
- no fixed polling is required for idle systems
- due jobs are driven by persisted `next_due_at`
- restart recovery still works

- [ ] **Step 2: Run tests to verify gaps**

Run: `xmake run test-automation`
Expected: FAIL until legacy code paths are removed or redirected

- [ ] **Step 3: Write minimal cleanup implementation**

Delete or reduce:
- legacy mixed persistence paths
- old `collect_due()`-style scanning
- old polling scheduler logic

Keep only compatibility shims that still serve real callers.

- [ ] **Step 4: Run final targeted verification**

Run: `xmake run test-automation`
Expected: PASS

- [ ] **Step 5: Run binary smoke test**

Run: `xmake build orangutan`
Expected: BUILD PASS

- [ ] **Step 6: Commit**

```bash
git add src/automation/repository.hpp src/automation/repository.cpp src/automation/model.hpp src/automation/model.cpp tests/automation/automation-repository-test.cpp tests/automation/automation-service-runtime-test.cpp
git commit -m "refactor: remove legacy automation polling model"
```

## Notes for Implementers

- Keep commits focused and task-scoped.
- Do not rebuild the whole project after every tiny edit; prefer `test-automation` until the final smoke build.
- New SQLite code must use the expected-based API, not the throwing wrapper.
- The driver is the only layer allowed to know about sleeping and task-pool scheduling.
- The kernel must stay thread-free and deterministic under test.
- Preserve current CLI/web/channel-visible behavior even if internal type names change.
