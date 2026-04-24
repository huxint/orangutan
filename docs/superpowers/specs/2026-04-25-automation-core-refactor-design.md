# Automation Core Refactor Design

Date: 2026-04-25
Status: Approved design baseline
Scope: Redesign Orangutan automation scheduling, storage, and execution as an embeddable C++ core with a thin optional runtime driver and a fluent DSL.

## Summary

Orangutan's current automation subsystem mixes job definition, schedule state, execution state, and background runtime control into a single service/runtime stack. The immediate CPU bug surfaced in `src/automation/runtime.cpp`, but the deeper issue is architectural: automation is modeled as a permanently alive polling subsystem instead of a compact scheduling kernel.

This design replaces the current model with:

- a thread-free automation core
- explicit job definition and schedule state persistence
- a store optimized for `next_due_at` and due-job reservation
- a thin optional runtime driver that sleeps until the next due time
- a fluent modern C++ DSL layered above the core
- adapter-based execution so Orangutan internals and future external integrations use the same kernel

The result should be efficient, modern, concise, restart-safe, and easy to embed as a C++ library API.

## Goals

- Keep existing automation functionality available after migration.
- Make automation primarily a C++ library API, not a background module.
- Remove fixed-interval polling from the core model.
- Persist job definitions and the minimum schedule state needed for precise restart recovery.
- Support per-job missed-run policies:
  - `skip`
  - `run_once`
  - `catch_up`
- Support a fluent modern C++ DSL without coupling scheduler internals to workflow composition.
- Keep the system efficient in CPU, wakeups, memory footprint, and database access patterns.
- Make external integration straightforward through adapter boundaries.

## Non-Goals

- This is not an event-sourcing redesign.
- This is not a generic workflow engine.
- This is not a distributed scheduler in the first version, though lightweight lease-based coordination is supported.
- This redesign does not require preserving the current internal class names or call surface, only the user-visible capabilities.

## Root Cause

### Immediate bug

`src/automation/runtime.cpp` builds a repeating timer pipeline incorrectly, which can cause the runtime to spin after the first wakeup instead of waiting for a fresh timer each cycle.

### Architectural root cause

The larger issue is that the subsystem is fundamentally modeled around a live polling loop:

- `AutomationRuntime` owns a background scheduling loop.
- `AutomationService::collect_due()` loads enabled jobs and filters them in memory.
- startup normalization scans and rewrites state eagerly
- channel mode starts automation runtime even when no jobs need scheduling

This yields unnecessary wakeups, extra SQLite reads, and a runtime that stays active even while idle.

## Design Principles

- Value semantics first
- Explicit state transitions
- Thin interfaces with strong ownership boundaries
- No hidden background work in the core
- Persistence model optimized for scheduling queries
- DSL is syntax sugar over stable model objects, not business logic
- Execution and delivery are adapters, not scheduler responsibilities
- Modern C++23 style:
  - `std::expected`
  - `std::variant`
  - strong typed IDs
  - policy objects
  - small composable classes

## Architecture

The redesign splits automation into five layers:

1. `automation::model`
   Pure value objects. No IO, threads, or runtime dependencies.

2. `automation::store`
   Persistence and indexed query layer. No scheduling policy decisions.

3. `automation::kernel`
   Thread-free scheduling kernel. Computes what should happen next and advances schedule state.

4. `automation::driver`
   Optional thin runtime driver. Handles sleeping, wakeup, stop, and dispatch.

5. `automation::dsl`
   Fluent C++ API for constructing jobs and action bindings.

High-level flow:

`DSL -> Model -> Store + Kernel -> Driver(optional) -> Executor Adapter`

## Core Model

### `JobId` and `ExecutionId`

Use strong typed IDs instead of plain strings throughout the kernel API.

Example direction:

```cpp
struct JobId {
    std::string value;
};

struct ExecutionId {
    std::string value;
};
```

### `ScheduleSpec`

Replace the current tagged struct with a `std::variant`.

```cpp
struct CronSchedule {
    std::string expr;
    std::string time_zone = "UTC";
};

struct IntervalSchedule {
    std::chrono::seconds every;
    std::chrono::seconds jitter{0};
    std::vector<ActiveWindow> active_windows;
    std::string time_zone = "UTC";
};

struct OneShotSchedule {
    std::chrono::system_clock::time_point at;
};

using ScheduleSpec = std::variant<CronSchedule, IntervalSchedule, OneShotSchedule>;
```

This removes invalid cross-field combinations and lets the compiler enforce schedule shape.

### `ActionDescriptor`

The scheduler should not persist arbitrary callables. Instead, it persists an action key plus serializable payload.

```cpp
struct ActionDescriptor {
    std::string action_key;
    nlohmann::json payload;
};
```

Concrete behavior is resolved later through an `ActionRegistry`.

### `ExecutionPolicy`

Execution behavior is part of the model and should be explicit.

```cpp
enum class missed_run_policy : std::uint8_t {
    skip,
    run_once,
    catch_up,
};

enum class overlap_policy : std::uint8_t {
    forbid,
    queue_one,
    parallel,
};

struct RetryPolicy {
    int max_attempts = 0;
    std::chrono::milliseconds initial_backoff{0};
    std::chrono::milliseconds max_backoff{0};
};

struct ExecutionPolicy {
    missed_run_policy missed_runs = missed_run_policy::run_once;
    overlap_policy overlap = overlap_policy::forbid;
    RetryPolicy retry;
};
```

### `ResultPolicy`

Result handling remains configurable but is not allowed to leak into scheduling rules.

```cpp
struct ResultPolicy {
    delivery_mode mode = delivery_mode::silent;
    std::vector<std::string> targets;
    bool persist_full_reply = true;
};
```

### `JobDefinition`

This is the stable "what the job is" record.

```cpp
struct JobDefinition {
    JobId id;
    std::string key;
    ScheduleSpec schedule;
    ActionDescriptor action;
    ExecutionPolicy execution;
    ResultPolicy result;
    nlohmann::json metadata;
    std::int64_t version = 0;
};
```

### `ScheduleState`

This is the mutable "what state the scheduler is in" record.

```cpp
struct ScheduleState {
    bool enabled = true;
    bool paused = false;
    std::optional<std::int64_t> next_due_at;
    std::optional<std::int64_t> last_scheduled_at;
    std::optional<std::int64_t> last_started_at;
    std::optional<std::int64_t> last_finished_at;
    std::string last_status;
    int in_flight_count = 0;
    std::string lease_owner;
    std::optional<std::int64_t> lease_expires_at;
    std::int64_t revision = 0;
};
```

### `DispatchRequest`

The kernel does not execute jobs directly. It emits dispatch requests.

```cpp
enum class dispatch_reason : std::uint8_t {
    scheduled,
    catch_up,
    manual,
    resumed,
};

struct DispatchRequest {
    JobId job_id;
    ExecutionId execution_id;
    std::chrono::system_clock::time_point scheduled_for;
    dispatch_reason reason = dispatch_reason::scheduled;
    ActionDescriptor action;
    ExecutionPolicy execution;
    ResultPolicy result;
};
```

### `ExecutionRecord`

Persistent record of what happened during one execution.

```cpp
struct ExecutionRecord {
    ExecutionId execution_id;
    JobId job_id;
    std::int64_t scheduled_for = 0;
    std::int64_t started_at = 0;
    std::optional<std::int64_t> finished_at;
    std::string status;
    std::string summary;
    std::string reply_ref;
    std::string delivery_status;
    std::string driver_id;
};
```

## Persistence Model

Split persistence into separate records for definition and mutable schedule state.

### `automation_job_definitions`

- `job_id`
- `job_key`
- `schedule_kind`
- `schedule_json`
- `action_key`
- `action_payload_json`
- `execution_policy_json`
- `result_policy_json`
- `metadata_json`
- `created_at`
- `updated_at`
- `version`

### `automation_job_state`

- `job_id`
- `enabled`
- `paused`
- `next_due_at`
- `last_scheduled_at`
- `last_started_at`
- `last_finished_at`
- `last_status`
- `in_flight_count`
- `lease_owner`
- `lease_expires_at`
- `revision`
- `updated_at`

### `automation_executions`

- `execution_id`
- `job_id`
- `scheduled_for`
- `dispatch_reason`
- `attempt`
- `started_at`
- `finished_at`
- `status`
- `summary`
- `reply_ref`
- `delivery_status`
- `driver_id`

### `automation_deliveries`

Persist notification-side effects by `execution_id`, not by direct scheduler coupling.

### Key indexes

- `job_definitions(job_key)` unique
- `job_state(enabled, paused, next_due_at)`
- `job_state(lease_expires_at)`
- `executions(job_id, started_at DESC)`

This enables:

- efficient `next_due_at()` queries
- efficient `list_due(now, limit)` queries
- recovery of expired claims
- no full-table definition scans during normal scheduling

## Store Interface

The store owns persistence and SQL shape. The kernel should depend on an interface, not on raw SQLite details.

Example direction:

```cpp
class JobStore {
public:
    virtual auto save_job(const JobDefinition&, const ScheduleState&) -> std::expected<void, StoreError> = 0;
    virtual auto load_job(const JobId&) const -> std::expected<std::optional<std::pair<JobDefinition, ScheduleState>>, StoreError> = 0;
    virtual auto remove_job(const JobId&) -> std::expected<void, StoreError> = 0;

    virtual auto next_due_at() const -> std::expected<std::optional<std::int64_t>, StoreError> = 0;
    virtual auto list_due(std::int64_t now, std::size_t limit) const -> std::expected<std::vector<JobId>, StoreError> = 0;
    virtual auto reserve_due(std::int64_t now, std::size_t limit, std::string_view driver_id, std::int64_t lease_until)
        -> std::expected<std::vector<std::pair<JobDefinition, ScheduleState>>, StoreError> = 0;

    virtual auto record_started(const ExecutionRecord&) -> std::expected<void, StoreError> = 0;
    virtual auto record_finished(const ExecutionRecord&) -> std::expected<void, StoreError> = 0;
};
```

Repository implementations should use the project's `std::expected` SQLite API for new code.

## Kernel API

The kernel is the scheduling brain. It is thread-free and does not sleep.

Example shape:

```cpp
class Kernel {
public:
    auto upsert_job(JobDefinition, std::optional<ScheduleState>) -> std::expected<JobId, KernelError>;
    auto remove_job(const JobId&) -> std::expected<void, KernelError>;
    auto pause_job(const JobId&) -> std::expected<void, KernelError>;
    auto resume_job(const JobId&, TimePoint now) -> std::expected<void, KernelError>;

    auto next_wakeup() const -> std::expected<std::optional<TimePoint>, KernelError>;
    auto reserve_due(TimePoint now, std::size_t limit, std::string_view driver_id)
        -> std::expected<std::vector<DispatchRequest>, KernelError>;

    auto mark_started(const ExecutionId&, TimePoint now) -> std::expected<void, KernelError>;
    auto mark_finished(const ExecutionId&, const ExecutionResult&, TimePoint now) -> std::expected<void, KernelError>;
    auto recover(TimePoint now, std::string_view driver_id) -> std::expected<void, KernelError>;
};
```

The critical method is `reserve_due()`, not `collect_due()`. It must atomically:

- find due jobs
- apply missed-run policy
- generate dispatch requests
- claim state for execution

This prevents double dispatch and enables future multi-driver scenarios.

## Missed-Run Semantics

Each job carries its own missed-run policy.

### `skip`

If multiple scheduled times were missed while the process was down, drop them and advance to the next valid schedule point.

### `run_once`

If one or more executions were missed, dispatch exactly one recovery run and then advance schedule state.

### `catch_up`

Generate recovery dispatches for each missed schedule point, subject to configurable caps:

- max catch-up depth per reservation cycle
- max wall-clock catch-up window
- overlap policy constraints

This avoids infinite backlog storms after long downtime.

## Overlap and In-Flight Policy

Overlap handling is explicit and enforced through schedule state.

### `forbid`

Do not start a new execution if another is in flight.

### `queue_one`

Remember at most one pending execution request while work is already running.

### `parallel`

Allow multiple concurrent executions for the same job. This should be opt-in only.

## Lightweight Lease and Recovery Model

To support robust restart recovery and future external drivers, use soft leases.

When a driver reserves due jobs:

- `lease_owner` is set
- `lease_expires_at` is set
- state revision is incremented

When execution begins:

- `in_flight_count` is incremented
- an execution record is written

When execution finishes:

- `in_flight_count` is decremented
- `next_due_at` is advanced
- lease is cleared or refreshed as appropriate
- final execution and delivery state are written

On recovery:

- expired leases are reclaimed
- incomplete execution records can be marked as interrupted or retried according to policy

This is deliberately simpler than distributed consensus, but significantly safer than process-local bookkeeping.

## Runtime Driver

The runtime driver is optional and intentionally thin.

Responsibilities:

- query `next_wakeup()`
- sleep until the earliest due time
- wake early when jobs are added, removed, paused, or resumed
- reserve due dispatches
- invoke executor adapter
- report started and finished execution

Non-responsibilities:

- no global job cache
- no fixed-interval poll loop
- no direct business logic
- no ownership of delivery semantics

Idle systems should remain asleep with no recurring tick.

## Executor and Adapter Model

Scheduling must be decoupled from Orangutan-specific work.

### `ActionRegistry`

An action registry resolves persisted `action_key + payload` into executable behavior.

```cpp
class ActionRegistry {
public:
    template <typename Payload, typename Fn>
    void add(std::string key, Fn&& fn);
};
```

### `ExecutorPort`

```cpp
struct ExecutionContext {
    JobId job_id;
    ExecutionId execution_id;
    TimePoint scheduled_for;
    std::stop_token stop_token;
};

class ExecutorPort {
public:
    virtual auto dispatch(const DispatchRequest&, const ExecutionContext&)
        -> std::expected<ExecutionResult, ExecutionError> = 0;
};
```

Orangutan-specific actions such as agent prompts, heartbeat checks, or channel notifications become adapter registrations rather than scheduler internals.

## Fluent DSL

The DSL should build stable model objects while staying separate from scheduling internals.

Example shape:

```cpp
auto job = automation::job("repo-sync")
    .schedule(automation::cron("0 * * * *").time_zone("Asia/Shanghai"))
    .missed_run(automation::missed_run_policy::run_once)
    .overlap(automation::overlap_policy::forbid)
    .retry(automation::retry_policy::exponential(3, 1s, 30s))
    .deliver_to("cli")
    .bind("agent.prompt", agent_prompt_payload{
        .agent = "default",
        .prompt = "scan repo and summarize changes",
    })
    .build();
```

Guidelines:

- builder returns `std::expected<JobDefinition, BuildError>`
- typed payloads serialize through codecs
- DSL does not own persistence or runtime

## Chained Execution

Chain syntax is supported, but it belongs to the action layer, not the scheduler.

Example:

```cpp
auto pipeline = automation::pipeline()
    .step("fetch", fetch_payload{...})
    .then("summarize", summarize_payload{...})
    .then("notify", notify_payload{...});
```

The pipeline is compiled into a single persisted `ActionDescriptor` such as:

- `action_key = "pipeline"`
- `payload = {... steps ...}`

This keeps the scheduler simple while allowing expressive workflow composition above it.

## Migration Plan

### Phase 1: Introduce new core types

- add new model types
- add store interfaces and SQLite implementation
- add kernel skeleton
- do not remove existing automation APIs yet

### Phase 2: Add runtime driver

- implement sleep-until-next-due driver
- implement wakeup on mutation
- route execution through adapter interfaces

### Phase 3: Compatibility bridge

- make current `AutomationService` and related callers delegate to the new kernel/store where feasible
- preserve current CLI/web/channel/heartbeat functionality

### Phase 4: Remove legacy runtime model

- delete polling runtime
- delete legacy mixed `Automation` persistence shape
- migrate tests to the new API

## Testing Strategy

### Unit tests

- schedule computation
- missed-run policy semantics
- overlap policy semantics
- lease expiration and recovery
- DSL builder validation
- payload codec round trips

### Store tests

- `next_due_at()` correctness
- `list_due(now, limit)` correctness
- atomic reservation behavior
- recovery of expired leases

### Driver tests

- sleeps when idle
- wakes on job mutation
- does not tick repeatedly without due work
- executes and records runs correctly

### Integration tests

- restart recovery with persisted state
- compatibility with heartbeat/category execution
- delivery persistence and acknowledgement flow

## Why This Design

This design solves both the immediate and structural problems:

- it eliminates fixed recurring polling from the core
- it stops loading all enabled jobs every scheduler cycle
- it keeps idle memory and CPU usage low
- it supports restart-safe scheduling
- it provides a clear API for C++ embedding
- it preserves room for future HTTP, webhook, or plugin adapters without making them kernel dependencies

## Implementation Guidance

- Prefer `std::expected` over exceptions at API boundaries.
- Keep model types trivially understandable and value-oriented.
- Use `std::variant` rather than enum-plus-unused-fields for schedule kinds.
- Keep the runtime driver very small and disposable.
- Make SQL query shape part of the design, not an afterthought.
- Keep the DSL expressive but shallow.
- Avoid turning scheduling concerns into action execution concerns.

## Open Follow-Up

The next artifact after this design should be a concrete implementation plan that breaks the work into:

- model/store introduction
- kernel state machine
- driver implementation
- compatibility bridge
- migration and cleanup
