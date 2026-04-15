# Chainable API Modernization Design

**Date:** 2026-04-15

**Status:** approved for planning

## Goal

Standardize the Orangutan project on a single canonical fluent API pattern using C++23 deducing `this`, `std::expected` for fallible operations, and `[[nodiscard]]` on terminal methods. The result should be consistent across all builder and configuration modules, explicit in error handling, and free of legacy `return *this` chainable patterns and inconsistent error return styles.

## Scope

In scope:

- `src/types/` — Conversation chainable migration
- `src/tools/` — ToolSpecBuilder, ToolDispatch, ContextualToolGroup
- `src/channel/` — QqMessageBuilder
- `src/automation/` — AutomationBuilder
- `src/agent/` — new AgentLoopBuilder
- `src/skills/` — SkillLoader factory and chainable config
- `src/web/` — WebServer factory and chainable config
- `src/coordinator/` — CoordinatorManager chainable configuration
- clang-tidy fixes and cross-cutting modernization in affected files
- tests for all migrated modules

Out of scope:

- `src/utils/` — free functions, already clean
- `src/bootstrap/` — DI container, not builder-appropriate
- `src/config/` — static factories, already well-designed
- `src/permissions/` — allow()/deny()/ask() pattern already elegant
- `src/process/`, `src/hooks/`, `src/heartbeat/` — too minimal
- `src/memory/`, `src/storage/` — CRUD services, side-effectful void returns correct
- `src/prompt/` — utility functions
- `src/swarm/` — service methods, each operation independent
- async/stdexec migration (separate future effort)
- pipe operator composition (too invasive)

## Problem Statement

### 1. Inconsistent chainable API styles across modules

Some modules (`providers/RequestBuilder`, `types/Message`) already use modern C++23 deducing `this` for chainable APIs, while others (`AutomationBuilder`, `QqMessageBuilder`, `ToolSpecBuilder`, `ToolDispatch`, `ContextualToolGroup`, `Conversation`) use traditional `return *this`. Several modules that would benefit from chainable configuration (`AgentLoop`, `SkillLoader`, `WebServer`, `CoordinatorManager`) still use void-returning setters. This inconsistency makes the codebase harder to learn and harder to compose.

### 2. Mixed error handling conventions

Error handling across builder and dispatch surfaces mixes `bool`, `std::optional`, and `std::expected` inconsistently. Callers cannot rely on a single pattern for checking construction or dispatch results. Some builders silently produce invalid objects rather than returning an error at the build boundary.

### 3. Unaddressed clang-tidy warnings

Several modules carry suppressible modernization warnings (`performance-enum-size`, `modernize-use-emplace`, `readability-use-anyofallof`, `readability-redundant-member-init`) and legacy idioms (`map.find() != map.end()`, `std::sort(begin, end)`, C-style casts) that conflict with the project's modern C++23 baseline.

## Design

### Canonical fluent API pattern

Every chainable class follows one pattern. Chainable methods use deducing `this` and forward the self parameter. Terminal methods are marked `[[nodiscard]]`. Fallible terminals return `std::expected<T, std::string>`. Infallible terminals return the product directly. Static factory entry points use `ClassName::create()` or `ClassName::named("x")`.

Reference implementation:

```cpp
class SomeBuilder {
    auto with_name(this auto &&self, std::string_view name) -> decltype(auto) {
        self.name_ = std::string(name);
        return std::forward<decltype(self)>(self);
    }

    [[nodiscard]]
    auto build() const -> std::expected<Product, std::string> {
        if (name_.empty()) {
            return std::unexpected("name is required");
        }
        return Product{.name = name_};
    }
};
```

Pattern rules:

- chainable methods take `this auto &&self` and return `std::forward<decltype(self)>(self)`
- terminal methods are `const` where possible and marked `[[nodiscard]]`
- fallible terminals return `std::expected<T, std::string>`
- infallible terminals return the product directly
- static factory entry points begin the chain

### Module refactoring scope and ordering

Refactoring proceeds bottom-up so that lower layers stabilize before higher layers depend on them.

**Layer 1 — types/**

Conversation migrates to deducing `this`. Message is already exemplary and requires no changes.

**Layer 2 — tools/**

ToolSpecBuilder, ToolDispatch, and ContextualToolGroup migrate to deducing `this`. ToolSpecBuilder::build() and ToolDispatch::run() gain `[[nodiscard]]` and return `std::expected`.

**Layer 3 — channel/**

QqMessageBuilder migrates to deducing `this`. build() gains `[[nodiscard]]` but remains infallible.

**Layer 4 — automation/**

AutomationBuilder migrates to deducing `this`. build() returns `std::expected<Automation, std::string>`.

**Layer 5 — agent/**

A new AgentLoopBuilder wraps the current void-setter configuration surface. Target usage:

```cpp
auto loop = AgentLoop::configure(provider, route)
    .with_thinking_budget(512)
    .with_environment_info(env)
    .build();
```

**Layer 6 — services** (builder-appropriate only)

- SkillLoader: `create()` factory, chainable `with_source()` and `with_workspace_root()`. Terminal `build()` returns `std::expected`.
- WebServer: `create()` factory, chainable config setters. `start()` and `stop()` stay non-chainable.
- CoordinatorManager: chainable configuration builder. Runtime mutation stays void.

### Error handling standardization

Methods gaining `std::expected`:

| Method | Current Return | New Return |
|--------|---------------|------------|
| AutomationBuilder::build() | Automation | std::expected\<Automation, std::string\> |
| ToolDispatch::run() | ToolResult | std::expected\<ToolResult, std::string\> |
| ToolSpecBuilder::build() | ToolSpec | std::expected\<ToolSpec, std::string\> |
| AgentLoopBuilder::build() | AgentLoop | std::expected\<AgentLoop, std::string\> |
| SkillLoader::build() (new) | N/A | std::expected\<SkillLoader, std::string\> |
| WebServer::build() (new) | N/A | std::expected\<WebServer, std::string\> |

Where `std::expected` does not apply:

- infallible setters and simple getters
- QqMessageBuilder::build() (infallible)
- Conversation::append() (infallible)
- runtime CRUD operations

Error string convention: all lowercase, no trailing period, concise.

Caller pattern:

```cpp
auto result = Automation::named("backup")
    .for_agent("worker1")
    .run_prompt("check status")
    .cron("0 9 * * *")
    .build();

if (!result.has_value()) {
    logger->error("failed to build automation: {}", result.error());
    return;
}
auto automation = std::move(result).value();
```

### clang-tidy fixes and cross-cutting modernization

Specific warnings to fix in affected files:

- `performance-enum-size`: `response_stop_reason` and others gain explicit `uint8_t` or `uint16_t` base types
- `modernize-use-emplace`: `push_back` becomes `emplace_back` in `automation/builder.cpp` and similar sites
- `readability-use-anyofallof`: manual loops become `std::ranges::all_of` / `std::ranges::any_of`
- `readability-redundant-member-init`: remove redundant `= {}` / `= 0` initializers

Proactive modernization across touched files:

- `map.find(k) != map.end()` becomes `map.contains(k)`
- `std::sort(v.begin(), v.end())` becomes `std::ranges::sort(v)`
- C-style casts become `static_cast<T>(x)`
- missing `explicit` on single-argument constructors is added
- `std::bind` becomes lambda
- `__builtin_unreachable()` becomes `std::unreachable()`

## Testing Strategy

Per module, test:

1. **Chainable API fluency** — chain compiles and produces correct state
2. **std::expected error paths** — invalid states produce meaningful error strings
3. **Rvalue/lvalue correctness** — deducing `this` works with both temporaries and persistent builders
4. **clang-tidy regression** — no new warnings introduced

Existing tests to update:

- `tests/automation/automation-model-test.cpp`
- `tests/tools/`
- `tests/channel/`

New tests:

- AgentLoopBuilder construction and error paths
- error paths for every builder gaining `std::expected`

## Non-Goals

- No async/stdexec migration — that is a separate future effort
- No pipe operator composition — too invasive for the current refactoring scope
- No changes to excluded modules listed above

## Implementation Sequence

1. Migrate `Conversation` to deducing `this` in `src/types/`.
2. Migrate `ToolSpecBuilder`, `ToolDispatch`, and `ContextualToolGroup` in `src/tools/`.
3. Migrate `QqMessageBuilder` in `src/channel/`.
4. Migrate `AutomationBuilder` in `src/automation/`.
5. Introduce `AgentLoopBuilder` in `src/agent/`.
6. Add factory and chainable patterns to `SkillLoader`, `WebServer`, and `CoordinatorManager`.
7. Apply clang-tidy fixes and cross-cutting modernization to all touched files.
8. Update existing tests and add new tests for all migrated modules.

## Acceptance Criteria

- all chainable modules use deducing `this` with `std::forward<decltype(self)>(self)` return
- no module uses traditional `return *this` for chainable APIs
- fallible terminal methods return `std::expected<T, std::string>` and are marked `[[nodiscard]]`
- error strings are all lowercase, concise, and have no trailing period
- clang-tidy produces no new warnings in affected files
- all existing tests pass after migration
- new tests cover chainable fluency, error paths, and rvalue/lvalue correctness for every migrated module
