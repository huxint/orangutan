# Core Loop Follow-Ups Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Continue simplifying Orangutan's core ReAct execution path after the default prompt cache landed.

**Architecture:** Preserve the existing core boundary: `AgentLoop` drives turns, `ToolRegistry` dispatches tools, `ProviderSystem` sends provider requests, and protocol adapters own vendor JSON. Future work should extract small, testable units around hot paths without changing observable runtime behavior.

**Tech Stack:** C++23, xmake, Catch2, stdexec sender/receiver at async boundaries, `std::expected` where new boundary APIs are introduced.

---

## Current State

The first prompt-cost optimization is implemented:

- `agent::AgentLoop` now caches the rendered default system prompt as a runtime snapshot.
- `AgentLoop::set_environment_info(...)` refreshes that cache.
- `detail::build_system_prompt(...)` receives the pre-rendered default prompt.
- Dynamic sections remain dynamic: active skills refresh each ReAct iteration, deferred tool summaries refresh each iteration, and memory recall remains once per user turn.

The remaining work below is intentionally split into independent chunks. Implement one chunk at a time and commit each completed chunk separately.

## Why Continue

The current architecture is healthy, but several core-chain hotspots still create repeated work or cognitive overhead:

- `AgentLoop::run` coordinates too many responsibilities in one function.
- Tool execution uses synchronous sender pipelines that are immediately waited, which obscures a sequential lifecycle.
- Provider fallback policy is embedded inside `RuntimeBackend`, making sticky-target and stream-failure behavior harder to test.
- Protocol adapters repeat JSON parsing and stream event emission boilerplate.
- Prompt assembly now caches the default section, but future cacheable prompt pieces need an explicit extension point rather than ad hoc fields.

The guiding rule: keep runtime behavior stable unless a test and design explicitly approve a behavior change.

## Chunk 1: Add a Prompt Snapshot Boundary

### Why

The current `default_system_prompt_` field solves the immediate repeated default prompt rebuild. If more prompt pieces become cacheable, adding more fields directly to `AgentLoop` will spread prompt assembly concerns across the loop.

This chunk introduces a small boundary only after the first cache has proven useful. It should not cache dynamic sections yet.

### Design

Create a tiny value type for invariant prompt state. Keep it boring:

```cpp
struct PromptBaseSnapshot {
    std::string default_system_prompt;
};
```

Possible location:

- `src/agent/agent-loop-prompt.hpp`

Keep all dynamic rendering in `agent-loop.cpp` and `agent-loop-memory.hpp` for now.

### Files

- Create: `src/agent/agent-loop-prompt.hpp`
- Modify: `src/agent/agent-loop.hpp`
- Modify: `src/agent/agent-loop.cpp`
- Modify: `src/agent/agent-loop-memory.hpp`
- Test: `tests/agent/agent-loop-test.cpp`

### Steps

- [ ] **Step 1: Write a regression test proving behavior stays unchanged**

Use the existing tests as the behavior contract. Add no new behavior if the existing cache snapshot test already covers the new type. If adding a test, assert only externally visible prompt content.

Run:

```bash
xmake run test-agent "default prompt cache uses runtime snapshots refreshed by environment updates"
```

Expected: PASS before refactor and after refactor.

- [ ] **Step 2: Introduce the value type**

Add `src/agent/agent-loop-prompt.hpp`:

```cpp
#pragma once

#include <string>

namespace orangutan::agent::detail {

    struct PromptBaseSnapshot {
        std::string default_system_prompt;
    };

} // namespace orangutan::agent::detail
```

- [ ] **Step 3: Replace `default_system_prompt_` with the snapshot**

In `AgentLoop`, replace:

```cpp
std::string default_system_prompt_;
```

with:

```cpp
detail::PromptBaseSnapshot prompt_base_;
```

Update `refresh_default_system_prompt()` to assign `prompt_base_.default_system_prompt`.

- [ ] **Step 4: Keep prompt build call explicit**

`AgentLoop::run` should still pass only `prompt_base_.default_system_prompt` into `detail::build_system_prompt(...)`. Do not pass the entire snapshot unless the build function needs it.

- [ ] **Step 5: Verify**

Run:

```bash
xmake run test-agent
```

Expected: all agent tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/agent/agent-loop-prompt.hpp src/agent/agent-loop.hpp src/agent/agent-loop.cpp src/agent/agent-loop-memory.hpp tests/agent/agent-loop-test.cpp
git commit -m "refactor: isolate agent prompt base snapshot"
```

## Chunk 2: Decompose AgentLoop::run Around a Turn Context

### Why

`AgentLoop::run` currently owns hook dispatch, history mutation, memory section rendering, skill refresh, incoming message injection, provider request construction, continuation handling, tool execution, and loop detection handling. It is readable but dense.

This chunk should make the turn lifecycle easier to audit without changing ordering.

### Design

Introduce an internal state struct in `src/agent/agent-loop.cpp` or a detail header if needed:

```cpp
struct TurnContext {
    std::string user_input;
    std::string memory_section;
    std::string final_text;
    bool human_output = false;
    detail::ToolCallCounts call_counts;
};
```

Keep it file-local if possible. Avoid creating a reusable abstraction until another file needs it.

Extract private helpers only when they reduce the size of `run` without hiding important sequencing:

- `begin_turn(...)`
- `refresh_effective_skills_prompt(...)`
- `send_iteration_request(...)`
- `append_tool_results(...)`

Do not extract continuation logic yet; it already lives in `agent-loop-history.hpp`.

### Files

- Modify: `src/agent/agent-loop.cpp`
- Optional Modify: `src/agent/agent-loop.hpp` if private methods are needed
- Test: `tests/agent/agent-loop-test.cpp`

### Required Ordering Contract

Keep this exact order:

1. `message_received` hook fires before appending provider-visible assistant output.
2. User input is appended and checkpointed.
3. Memory section renders once per user input before the loop.
4. Skills refresh at the start of each iteration.
5. Incoming messages inject before stop check and provider request.
6. Tool definitions are captured before prompt build/provider call.
7. Provider response is appended before tool execution.
8. Tool results are appended as a user message.
9. Loop warning message is appended after tool result checkpoint.
10. Final `message_sending` hook fires only when final text is non-empty.

### Steps

- [ ] **Step 1: Add an ordering-focused test if missing**

Review `run_checkpoints_tool_flow_and_result_application`, `incoming mailbox messages are injected between turns`, and `run_refreshes_skill_prompt_after_conditional_activation`.

If one ordering edge is not covered, add the smallest test. Example target: provider prompt count and history checkpoint order when incoming messages arrive on iteration 2.

Run the new focused test and confirm it fails only if your intended contract is not currently encoded.

- [ ] **Step 2: Add `TurnContext` locally**

Place it in the anonymous namespace of `src/agent/agent-loop.cpp` near `merge_refreshed_skill_prompt`.

- [ ] **Step 3: Move existing local variables into `TurnContext`**

Replace separate locals in `run`:

```cpp
detail::ToolCallCounts call_counts;
std::string final_text;
const bool human_output = ...;
const auto memory_section = ...;
```

with a single context value. Keep the logic otherwise unchanged.

- [ ] **Step 4: Extract only obvious helpers**

Start with a pure helper for skill prompt refresh:

```cpp
std::string effective_skills_prompt(std::string_view base_prompt, skills::SkillLoader *loader);
```

Preserve the current `skill_loader_ == nullptr` behavior.

- [ ] **Step 5: Run focused tests**

```bash
xmake run test-agent "run_checkpoints_tool_flow_and_result_application"
xmake run test-agent "incoming mailbox messages are injected between turns"
xmake run test-agent "run_refreshes_skill_prompt_after_conditional_activation"
```

Expected: all pass.

- [ ] **Step 6: Run full agent bucket**

```bash
xmake run test-agent
```

Expected: all agent tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/agent/agent-loop.cpp src/agent/agent-loop.hpp tests/agent/agent-loop-test.cpp
git commit -m "refactor: clarify agent turn context"
```

## Chunk 3: Simplify Sequential Tool Execution

### Why

`detail::execute_tools` in `src/agent/agent-loop-tools.hpp` wraps each tool call in a sender pipeline and immediately waits for it. That shape suggests asynchronous composition, but execution remains sequential. A named sequential lifecycle is easier to read and verify.

### Design

Keep public async boundaries unchanged. Only simplify the local per-tool implementation.

Replace the immediate sender pipeline with plain helper functions that preserve event order:

1. loop detection
2. terminal output
3. `tool_started` callback
4. before-tool hook
5. registry execution unless blocked or aborted
6. skill activation from touched paths
7. after-tool hook
8. `tool_finished` callback

Do not parallelize tools. Do not change abort behavior.

### Files

- Modify: `src/agent/agent-loop-tools.hpp`
- Test: `tests/agent/agent-loop-test.cpp`

### Steps

- [ ] **Step 1: Write a failing test for event order if needed**

If current tests do not assert callbacks around a tool call, add a test that records callback event names and verifies `tool_started` appears before `tool_finished`, and that `tool_finished` has a result.

Run:

```bash
xmake run test-agent "tool event callbacks preserve execution order"
```

Expected before implementation: either fail if test names new behavior not encoded, or pass if existing behavior already matches. If it passes, keep it as a guard for the refactor.

- [ ] **Step 2: Extract a small execution helper**

Suggested helper shape inside `agent::detail`:

```cpp
ToolResult execute_single_tool_call(
    const ToolUse &call,
    ToolRegistry &tools,
    HookManager *hook_manager,
    bool human_output,
    const AgentLoop::ToolEventCallback &on_tool_event,
    skills::SkillLoader *skill_loader
);
```

Keep loop-detection outside this helper if that keeps abort flow clearer.

- [ ] **Step 3: Remove the immediate sender pipeline**

Delete the `stdexec::just(...) | stdexec::then(...)` chain used only for one synchronous call. Keep includes tidy afterward.

- [ ] **Step 4: Verify abort behavior**

Run:

```bash
xmake run test-agent "run_aborts_after_fifth_identical_tool_call"
```

Expected: pass, with the side-effect tool still not executed.

- [ ] **Step 5: Verify full agent bucket**

```bash
xmake run test-agent
```

Expected: all agent tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/agent/agent-loop-tools.hpp tests/agent/agent-loop-test.cpp
git commit -m "refactor: simplify sequential tool execution"
```

## Chunk 4: Extract Provider Fallback Policy

### Why

Provider route/fallback behavior is embedded in `RuntimeBackend`. Current behavior includes deduping targets, skipping empty models, sticky preferred target selection, forward-only fallback, retryability checks, and stopping after streamed output. These are policy rules and should be directly testable without HTTP/protocol execution.

### Design

Extract a small internal component in provider execution.

Possible files:

- Create: `src/providers/execution/route-attempt-plan.hpp`
- Create: `src/providers/execution/route-attempt-plan.cpp`
- Modify: `src/providers/execution/runtime-backend.cpp`
- Test: add to existing provider execution/core tests under `tests/providers/`

Suggested API:

```cpp
namespace orangutan::providers::execution {

    class RouteAttemptPlan {
    public:
        RouteAttemptPlan(const ProviderRoute &route, std::string preferred_target_key);

        [[nodiscard]] bool empty() const;
        [[nodiscard]] const ModelTarget &current() const;
        [[nodiscard]] bool can_advance_after(const ProviderError &error, bool emitted_stream_event) const;
        void advance();
        [[nodiscard]] std::size_t current_index() const;
        [[nodiscard]] std::span<const ModelTarget> targets() const;

    private:
        std::vector<ModelTarget> targets_;
        std::size_t current_index_ = 0;
    };

} // namespace orangutan::providers::execution
```

If exposing this in a header feels too broad, keep it in an internal detail namespace and test through `RuntimeBackend`. Prefer direct tests if possible.

### Policy Contract

Preserve current behavior exactly:

- Primary target comes first.
- Fallback targets follow primary.
- Empty model targets are skipped.
- Duplicate targets are skipped by target key.
- If `preferred_target_key` matches a target, start there.
- If preferred target fails and it is the last target, do not wrap around to primary.
- Non-retryable errors stop fallback.
- Retryable errors can advance unless a stream event was already emitted.
- Streamed partial output prevents switching models.

### Steps

- [ ] **Step 1: Write route policy tests**

Add tests for:

- duplicate fallback dedupe
- preferred target starts in the middle
- preferred last target does not wrap to primary
- non-retryable error stops
- retryable error advances
- emitted stream event stops

Run the focused provider test target. Use the repository's target map to choose the relevant target, likely:

```bash
xmake run test-providers "route attempt plan"
```

If the target name differs, inspect `tests/providers/` and `xmake/tests.lua`.

- [ ] **Step 2: Extract route flattening and cursor logic**

Move `flatten_route`, `starting_index`, and advancement checks into `RouteAttemptPlan`.

- [ ] **Step 3: Wire `RuntimeBackend::execute` through the plan**

Keep `RuntimeBackend::attempt_target(...)`, `record_failure(...)`, and usage counters behavior unchanged at first. Then simplify only if tests remain green.

- [ ] **Step 4: Verify provider behavior**

Run:

```bash
xmake run test-providers
```

or the exact provider bucket used by this repository.

- [ ] **Step 5: Commit**

```bash
git add src/providers/execution/route-attempt-plan.hpp src/providers/execution/route-attempt-plan.cpp src/providers/execution/runtime-backend.cpp tests/providers
git commit -m "refactor: isolate provider fallback policy"
```

## Chunk 5: Centralize Protocol JSON Parsing Helpers

### Why

Protocol adapters repeat JSON parsing and exception-to-`ProviderError` mapping. That makes adding providers harder and risks inconsistent parsing errors.

### Design

Create a helper for parsing payloads into JSON objects while preserving current error category and labels.

Possible files:

- Create: `src/providers/protocols/protocol-json.hpp`
- Create: `src/providers/protocols/protocol-json.cpp`
- Modify: `src/providers/protocols/anthropic-messages.cpp`
- Modify: `src/providers/protocols/openai-chat-completions.cpp`
- Modify: `src/providers/protocols/openai-responses.cpp`
- Test: `tests/providers/protocols/protocol-adapters-test.cpp`

Suggested helper:

```cpp
[[nodiscard]]
nlohmann::json parse_protocol_json_object(
    std::string_view payload,
    std::string_view protocol_label,
    std::string_view context
);
```

The helper should throw `ProviderError{error_category::parsing, ...}`. Preserve enough label text that existing tests and logs remain useful, for example `openai responses response parse error`.

### Steps

- [ ] **Step 1: Add tests for malformed payload errors**

For each adapter, verify malformed JSON produces `error_category::parsing` and includes the adapter label in the message.

Run:

```bash
xmake run test-providers "protocol"
```

- [ ] **Step 2: Implement helper**

Keep it small. Do not introduce a generic protocol framework.

- [ ] **Step 3: Replace local parse helpers**

Update:

- `parse_anthropic_messages_payload`
- `parse_openai_chat_payload`
- `parse_openai_responses_payload`

Each can become a one-line wrapper around `parse_protocol_json_object(...)` if preserving function names reduces churn.

- [ ] **Step 4: Verify protocol tests**

```bash
xmake run test-providers
```

- [ ] **Step 5: Commit**

```bash
git add src/providers/protocols/protocol-json.hpp src/providers/protocols/protocol-json.cpp src/providers/protocols/anthropic-messages.cpp src/providers/protocols/openai-chat-completions.cpp src/providers/protocols/openai-responses.cpp tests/providers
git commit -m "refactor: share protocol json parsing"
```

## Chunk 6: Measure Before Provider Request Copy Optimizations

### Why

Provider requests currently copy history and tool definitions into `ProviderRequest`, then runtime backend copies the request again per attempt, then adapters serialize history into JSON. This might matter for long sessions, but optimizing without measurement risks making ownership and lifetime less safe.

### Design

Do not change `ProviderRequest` ownership yet. First add lightweight measurement around request construction and serialization in debug logs or a dedicated benchmark-style test utility.

### Files

- Modify: possibly `src/providers/provider.cpp`
- Modify: possibly `src/providers/execution/runtime-backend.cpp`
- Optional Create: `tests/providers/provider-request-cost-test.cpp`

### Steps

- [ ] **Step 1: Define what to measure**

At minimum:

- number of messages
- total text bytes in history
- number of tools
- serialized JSON payload size by adapter

- [ ] **Step 2: Add tests only for helper math**

If adding helper functions to estimate request size, test those helpers with simple message/tool fixtures.

- [ ] **Step 3: Add debug logging only if useful**

All `spdlog` messages must be lowercase. Do not log secrets or tool arguments that may contain secrets.

- [ ] **Step 4: Decide whether optimization is justified**

Only after measurement, consider any of:

- moving `effective_request = request` mutation to a smaller override object
- using shared snapshots for tool definitions
- keeping history ownership as-is for safety

- [ ] **Step 5: Commit measurement or close as unnecessary**

If no production change is justified, document the finding rather than changing ownership semantics.

## Cross-Cutting Verification

Run at least the relevant bucket after each chunk:

```bash
xmake run test-agent
```

For provider chunks, also run the matching provider test target. If uncertain, run:

```bash
xmake test
```

Be aware that full builds/tests are slow. Batch edits, then verify at clear checkpoints.

## Do Not Do

- Do not add provider-specific prompt caching until the local core loop is stable.
- Do not parallelize tool execution in these refactors.
- Do not change provider fallback semantics while extracting policy.
- Do not move large inline headers to `.cpp` in the same commit as behavioral refactors.
- Do not introduce new global state.
- Do not use `std::thread` or custom pools.
- Do not add compatibility shims for removed helper names unless an external consumer exists.

## References

- Current design spec: `docs/superpowers/specs/2026-04-27-prompt-base-cache-design.md`
- Agent loop: `src/agent/agent-loop.cpp`
- Prompt helpers: `src/agent/agent-loop-memory.hpp`
- Tool execution: `src/agent/agent-loop-tools.hpp`
- Provider runtime backend: `src/providers/execution/runtime-backend.cpp`
- Protocol adapters: `src/providers/protocols/`
- Agent tests: `tests/agent/agent-loop-test.cpp`
- Provider tests: `tests/providers/`
