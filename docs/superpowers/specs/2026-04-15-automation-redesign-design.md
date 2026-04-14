# Automation Redesign Design

**Date:** 2026-04-15

**Status:** approved for planning

## Goal

Replace the existing automation module with a first-generation architecture built around one modern C++23 automation model, one unified service API, one external protocol surface, and no compatibility layer for legacy task, heartbeat, inbox, schema, or test shapes.

The result should be:

- destructive by design rather than migration-oriented
- fluent and ergonomic for callers
- efficient in storage and execution
- explicit in ownership, state, and failure behavior
- easy to extend without re-splitting into parallel task and heartbeat stacks

## Scope

In scope:

- `src/automation/automation-types.*`
- `src/automation/automation-store.*`
- `src/automation/planner.*`
- `src/automation/scheduler.*`
- `src/automation/log-writer.*`
- `src/automation/cron-parser.*`
- `src/tools/automation/automation-tool-support.*`
- `src/tools/task/task-tool.cpp`
- `src/tools/heartbeat/heartbeat-tool.cpp`
- `src/tools/inbox/inbox-tool.cpp`
- web routes and bootstrap/runtime call sites that currently depend on `automation::Runtime` and `automation::Store`
- focused automation, tool, web, and integration tests that cover the new protocol and runtime

Out of scope:

- preserving `TaskSpec`, `HeartbeatSpec`, `InboxItem`, `Store`, or `Runtime` as public compatibility interfaces
- translating old sqlite schema or old JSON fields
- keeping separate `task`, `heartbeat`, and `inbox` tool contracts alive
- incremental migration code for old routes or old tests

## Problems To Address

### 1. The module is split by historical concepts instead of runtime semantics

The current design treats tasks and heartbeats as different top-level entities even though both are scheduled executions with the same action, delivery, and state concerns. That duplicates CRUD APIs, storage logic, validation paths, and tool surfaces.

### 2. CRUD, planning, execution, and delivery are tightly coupled

`automation::Runtime` currently mixes:

- storage-facing CRUD
- scheduling calculations
- worker lifecycle
- agent lease coordination
- execution result handling
- inbox delivery fallback

That makes the module harder to reason about and makes every new feature leak across multiple responsibilities.

### 3. The public API is process-oriented and not pleasant to compose

Callers work with mutable spec structs and operation-specific methods such as `save_task`, `save_heartbeat`, `run_task_now`, and `pause_heartbeat`. This is not a good fit for modern C++23 value types or fluent construction.

### 4. The external protocol is fragmented

The codebase currently exposes separate task, heartbeat, and inbox tools. The web and runtime layers mirror that split. This keeps historical distinctions alive in every consumer even after the module is unified internally.

### 5. The storage model bakes old categories into the schema

The current persistence shape encodes old entity boundaries and state fields directly. That makes it harder to evolve trigger kinds or delivery behavior without adding more legacy-specific columns and branching logic.

## Design

### Public model: one automation entity

The new core model is a single `Automation` value type. Old top-level concepts such as task and heartbeat disappear from the public interface.

`Automation` contains:

- stable identity
- agent ownership
- human-readable metadata
- one action definition
- one trigger definition
- one delivery policy
- runtime state
- tags and notes

The trigger shape is a tagged value rather than a class hierarchy explosion. The first generation supports:

- `cron`
- `interval`
- `once`

This keeps the model explicit and easy to persist while still leaving room for later extensions.

### Fluent API: builder-first, value-oriented, chainable

Construction should be ergonomic and immutable-in-style from the caller perspective. The builder owns validation and returns a finished `Automation` value.

Target usage:

```cpp
auto automation = automation::Automation::named("repo-check")
    .for_agent("default")
    .run_prompt("scan repo and summarize changes")
    .cron("0 9 * * *")
    .deliver_to("owner")
    .tag("daily")
    .enable()
    .build();
```

Interval example:

```cpp
using namespace std::chrono_literals;

auto automation = automation::Automation::named("pulse")
    .for_agent("default")
    .run_prompt("status check")
    .every(15min)
    .jitter(30s)
    .within_hours({9h, 18h})
    .deliver_silently()
    .build();
```

Builder rules:

- single-purpose, chainable methods
- explicit validation for required fields
- no hidden fallbacks to old shapes
- direct support for `std::chrono` durations where practical
- no macros and no implicit bool-style API tricks

### Top-level service: one stable operational boundary

The unified entry point is `AutomationService`.

The service owns the public operational API:

- `save(Automation automation)`
- `remove(std::string_view agent_key, std::string_view id_or_name)`
- `find(std::string_view agent_key, std::string_view id_or_name)`
- `list(const AutomationQuery &query)`
- `run_now(std::string_view agent_key, std::string_view id_or_name)`
- `pause(std::string_view agent_key, std::string_view id_or_name)`
- `resume(std::string_view agent_key, std::string_view id_or_name)`
- `list_runs(const RunQuery &query)`
- `list_deliveries(const DeliveryQuery &query)`
- `ack_delivery(std::string_view agent_key, std::string_view delivery_id)`
- `clear_deliveries(const DeliveryQuery &query)`

`AutomationService` exposes business semantics. It does not directly own planner math, raw sqlite details, or delivery implementation internals.

Mutation rules:

- `save(Automation automation)` is the canonical internal write boundary and always persists a fully validated, full-definition value
- protocol-level `create` requires a full automation definition
- protocol-level `update` and `PATCH /automation/{id}` are partial updates
- partial updates merge onto the stored definition before validation
- nested `trigger` and `delivery` objects are replace-on-write when present; they are not deep-merged field by field

This keeps the C++ service boundary simple while still allowing ergonomic patch-style external APIs.

### Identifier semantics: stable ids, agent-scoped unique names

The redesign keeps both machine ids and human-readable names, but their roles are distinct.

Rules:

- every automation has a stable generated `id`
- `name` must be unique within one `agent_key`
- create and update operations reject duplicate names within the same agent scope
- service and tool selectors may accept `id_or_name` for operator convenience
- selector resolution prefers exact `id` match first, then unique `name` match in the same `agent_key`
- web routes use path ids only and do not perform name-based lookup

This keeps CLI and tool workflows ergonomic without making HTTP routing ambiguous.

### Lifecycle semantics: enabled controls eligibility, paused controls temporary suspension

The module keeps both `enabled` and `paused`, but they must have non-overlapping meanings.

State meanings:

- `enabled=true` and `paused=false`
  The automation is active and eligible for scheduler evaluation.
- `enabled=true` and `paused=true`
  The automation is stored and visible, but the scheduler must skip it until resumed.
- `enabled=false` and `paused=false`
  The automation is disabled. The scheduler must skip it and `next_due_at` is cleared.
- `enabled=false` and `paused=true`
  This state is invalid and must not be persisted.

State rules:

- creation defaults to `enabled=true`, `paused=false`
- disabling an automation clears `paused` to `false` and clears `next_due_at`
- enabling an automation recomputes `next_due_at` from the current wall-clock time and leaves `paused=false`
- `pause` is valid only for enabled automations and sets `paused=true`
- `resume` is valid only for enabled automations and sets `paused=false`
- `run_now` may execute an automation regardless of `enabled` or `paused`, but it does not change lifecycle state

This gives planning a concrete transition model for service behavior, persistence normalization, and tests.

One-shot rules:

- a `once` automation is considered spent after its first execution attempt, regardless of success or failure
- both scheduled execution and `run_now` consume a one-shot automation
- the persisted spent marker is `last_run_at != null` together with `next_due_at = null`
- re-running a spent one-shot requires an explicit update that replaces the trigger with a new `once.at` value

This avoids ambiguous retry behavior and keeps one-shot state derivable from persisted runtime fields.

### Internal architecture: split by responsibility

The replacement module should be organized around small, explicit layers:

- `automation/model.*`
  Pure value types and enums.
- `automation/builder.*`
  Fluent construction and validation.
- `automation/parser.*`
  Cron parsing, duration parsing, time-window parsing, JSON input helpers.
- `automation/repository.*`
  SQLite persistence and query/filter helpers.
- `automation/planner.*`
  Due calculation and next-run planning.
- `automation/delivery.*`
  Delivery records and notifier integration.
- `automation/service.*`
  Public command/query API.
- `automation/runtime.*`
  Polling loop, execution orchestration, lease handling, and background scheduling.

Layer rules:

- `model` has no side effects
- `builder` depends on `model`
- `planner` depends on `model`
- `repository` depends on `model`
- `service` depends on `repository`, `planner`, and delivery/execution abstractions
- `runtime` depends on `service` internals plus execution coordination

This is the main structural change that prevents the next refactor from collapsing back into one large class.

### Storage model: unify around automation definitions and execution history

The sqlite schema should also be destructive and first-generation. It should not preserve old task and heartbeat storage shapes.

Recommended tables:

- `automations`
- `automation_runs`
- `automation_deliveries`

`automations` stores the canonical definition and runtime state:

- `id`
- `agent_key`
- `name`
- `enabled`
- `paused`
- `prompt`
- `notes`
- `tags_json`
- `trigger_json`
- `delivery_json`
- `last_run_at`
- `next_due_at`
- `last_status`
- `created_at`
- `updated_at`

`automation_runs` stores each execution:

- `id`
- `automation_id`
- `agent_key`
- `started_at`
- `finished_at`
- `status`
- `summary`
- `reply`
- `log_path`

`automation_deliveries` stores each delivery attempt or inbox-visible result:

- `id`
- `run_id`
- `automation_id`
- `agent_key`
- `target`
- `status`
- `title`
- `body`
- `created_at`
- `acked_at`

Design rules:

- persist trigger and delivery config as JSON rather than scattering legacy-specific columns
- keep frequently filtered operational fields such as `agent_key`, `enabled`, `paused`, and `next_due_at` as indexed columns
- treat inbox behavior as a delivery query over `automation_deliveries`, not as a separate top-level storage concept
- reject duplicate `(agent_key, name)` pairs at the repository boundary with a unique constraint

### Planning and scheduling: semantic trigger evaluation

`planner` operates on `Automation` values and returns due work plus the next known schedule state. It does not touch the database directly.

Responsibilities:

- validate and normalize trigger timing
- determine whether an automation is due
- compute `next_due_at`
- support `cron`, `interval`, and `once`
- respect active windows and pause state
- avoid startup backfill behavior unless the new semantics explicitly require it

The planner should favor existing STL and project helpers where possible:

- `std::chrono`
- ranges algorithms
- existing cron parser logic where it remains useful

### Trigger semantics: explicit clock basis and wire formats

Planning and protocol behavior must use one canonical timing model.

Clock rules:

- persisted runtime timestamps such as `last_run_at`, `next_due_at`, `started_at`, and `finished_at` use unix seconds in UTC
- external JSON fields intended for human interchange use explicit string formats rather than implicit local timestamps
- cron and active-window evaluation use the trigger's declared `time_zone`
- if `time_zone` is omitted, the canonical default is `UTC`

Canonical trigger wire formats:

- `cron`
  - `trigger.type = "cron"`
  - `trigger.cron = "0 9 * * *"`
  - `trigger.time_zone = "Asia/Shanghai"` or another IANA zone name
- `once`
  - `trigger.type = "once"`
  - `trigger.at = "2026-04-15T09:00:00Z"`
- `interval`
  - `trigger.type = "interval"`
  - `trigger.every = "15m"`
  - `trigger.jitter = "30s"`
  - `trigger.time_zone = "UTC"` when active windows are present
  - `trigger.active_windows = [{"start":"09:00","end":"18:00"}]`

Evaluation rules:

- `once` is due when the current UTC time is greater than or equal to `trigger.at` and the automation is not yet spent
- `cron` is evaluated against wall-clock time in `trigger.time_zone`
- `interval` cadence is computed from persisted UTC runtime state, while any active-window restriction is evaluated in `trigger.time_zone`
- interval scheduling is fixed-delay: after one run finishes, the next cycle is computed from that completion time rather than the prior planned due time
- active-window boundaries use a 24-hour `HH:MM` format and represent a half-open interval `[start, end)`
- interval `jitter` is a random positive offset sampled once per scheduling cycle when `next_due_at` is computed
- the sampled offset is preserved across restarts through the persisted `next_due_at` value and re-sampled only for the following cycle
- delayed or long-running interval executions do not create catch-up runs; they simply shift the next cycle forward from completion

These rules are intentionally concrete so planner logic, repository JSON shape, and tool/web contracts do not drift apart.

### Runtime and execution: orchestration only

`runtime` is a thin orchestration layer over the service and planner.

Responsibilities:

- start and stop the background scheduler
- fetch eligible automations
- acquire per-agent execution leases
- dispatch execution work
- persist run and delivery outcomes

Execution rules:

- continue using `stdexec` patterns for pipelines and scheduling
- prefer `stdexec`'s built-in thread-pool facilities instead of expanding custom thread management
- keep the per-agent lease concept, but move it behind the new runtime boundary
- do not mix CRUD methods into the runtime class

The runtime should be responsible for coordination, not for becoming a second service API.

### Delivery model: unify inbox and notifications

The old inbox concept becomes a query/view over delivery records.

Delivery behavior supports:

- silent recording only
- notification targets
- acknowledgement state on delivery records

This keeps the storage and external semantics aligned:

- a run creates zero or more delivery records
- callers query deliveries rather than a separate inbox entity
- acknowledgement updates delivery state in place

Delivery modes:

- `silent`
  - always creates an `automation_runs` row
  - creates no `automation_deliveries` rows
  - has nothing to acknowledge or clear
- `notify`
  - always creates an `automation_runs` row
  - creates one `automation_deliveries` row per resolved target
  - requires at least one delivery target
  - delivery rows remain queryable and acknowledgeable regardless of notification success or failure

Delivery clearing semantics:

- `ack_delivery` sets `acked_at` on one delivery row
- `clear_deliveries` bulk-acks all unacked delivery rows that match a `DeliveryQuery`
- `DeliveryQuery` used for clear must include `agent_key`; other filters reuse the same selection semantics as `list_deliveries`
- clear and ack operations never physically delete delivery history

### External protocol: one automation tool and one route family

The user-facing protocol becomes a unified `automation` tool.

Supported operations:

- `create`
- `update`
- `remove`
- `get`
- `list`
- `run`
- `pause`
- `resume`
- `list_runs`
- `list_deliveries`
- `ack_delivery`
- `clear_deliveries`

Suggested JSON shape:

- `id`
- `name`
- `agent_key`
- `prompt`
- `notes`
- `enabled`
- `tags`
- `trigger.type`
- `trigger.cron`
- `trigger.every`
- `trigger.jitter`
- `trigger.at`
- `trigger.time_zone`
- `trigger.active_windows`
- `delivery.mode`
- `delivery.targets`

The protocol does not preserve `task` and `heartbeat` vocabulary. Interval automation is the replacement for heartbeat behavior.

Web routes should mirror the same unified semantics:

- `GET /automation`
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

Route rules:

- web routes accept stable ids only in path parameters
- agent scoping remains explicit through auth/context or request parameters, not inferred from a human-readable name
- tool operations may continue accepting `id` or `name`, but HTTP endpoints use ids only for deterministic routing
- `DELETE /automation/deliveries` is a bulk-ack operation on the filtered delivery view rather than a physical delete

Canonical protocol examples:

Create:

```json
{
  "op": "create",
  "name": "repo-check",
  "prompt": "scan repo and summarize changes",
  "enabled": true,
  "tags": ["daily"],
  "trigger": {
    "type": "cron",
    "cron": "0 9 * * *",
    "time_zone": "Asia/Shanghai"
  },
  "delivery": {
    "mode": "notify",
    "targets": ["owner"]
  }
}
```

Update:

```json
{
  "op": "update",
  "id": "auto_123",
  "enabled": false,
  "delivery": {
    "mode": "silent",
    "targets": []
  }
}
```

In the update example, omitted top-level fields remain unchanged, while the provided `delivery` object replaces the previous delivery configuration entirely.

### Tests: replace, do not migrate

Old tests should not be preserved as historical compatibility artifacts. The new design deserves a new test layout that matches the new API and runtime boundaries.

Recommended suites:

- `tests/automation/automation-model-test.cpp`
- `tests/automation/automation-planner-test.cpp`
- `tests/automation/automation-repository-test.cpp`
- `tests/automation/automation-service-runtime-test.cpp`
- unified tool and web tests updated to call the `automation` protocol instead of separate task/heartbeat/inbox tools

Coverage priorities:

- builder validation and value semantics
- cron, interval, once, pause/resume, and active-window planning
- sqlite roundtrip for definitions, runs, and deliveries
- manual execution and background scheduling
- agent lease serialization
- delivery acknowledgement and clearing
- unified tool and web contract behavior

## Implementation Sequence

1. Replace public model and builder with the new unified `Automation` API.
2. Introduce the new repository and first-generation sqlite schema.
3. Rebuild planner logic around `cron`, `interval`, and `once`.
4. Introduce `AutomationService`, delivery handling, and runtime orchestration.
5. Replace tool and web surfaces with the unified `automation` protocol.
6. Delete legacy task, heartbeat, inbox, store, and runtime compatibility code.
7. Rewrite focused tests around the new model and service boundary.

This sequence keeps the model stable before the higher layers are rebuilt.

## Acceptance Criteria

- the module exposes one automation entity rather than separate task and heartbeat public models
- callers can construct automation definitions through a fluent, chainable API
- the public operational surface is a unified automation service
- sqlite storage uses a new schema centered on automations, runs, and deliveries
- planner logic supports cron, interval, and once triggers with explicit runtime state
- inbox semantics are represented through delivery records rather than a separate entity
- tool and web interfaces are unified under `automation`
- no compatibility layer remains for old automation structs, fields, routes, tools, or tests
- focused automation tests pass against the new first-generation architecture
