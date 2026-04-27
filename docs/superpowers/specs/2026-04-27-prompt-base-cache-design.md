# Prompt Base Cache Design

## Goal

Reduce repeated prompt construction work inside `agent::AgentLoop::run` while preserving current ReAct behavior. The first implementation should cache only the invariant default system prompt for an `AgentLoop` instance.

## Context

`AgentLoop::run` currently builds the effective system prompt on every ReAct iteration. The loop can run up to `MAX_ITERATIONS`, so any invariant work in prompt construction is multiplied per user turn.

Current flow in `src/agent/agent-loop.cpp`:

- Memory recall is already computed once per user turn with `detail::render_prompt_memory_section(memory_, user_input)`.
- Active skills are intentionally refreshed every iteration because tool execution may activate new skills.
- Deferred tool summaries are rebuilt every iteration because tool discovery can change the visible/deferred tool set.
- `detail::build_system_prompt(env_info_, effective_skills_prompt, *tools_, memory_section)` rebuilds the default system prompt every iteration.

`detail::build_system_prompt` in `src/agent/agent-loop-memory.hpp` calls `prompt::build_default_system_prompt(env_info)`. That function, implemented in `src/prompt/system-prompt-sections.cpp`, appends static sections, environment information, automation instructions, and workspace agent file contents from `.orangutan/agent/identity.md`, `.orangutan/agent/style.md`, and `.orangutan/agent/memory.md`.

The default system prompt is treated as a runtime snapshot. It includes values that can theoretically change while a long-lived runtime stays alive, including the current date, `$SHELL`, OS version, and workspace agent file contents. Re-reading those values every ReAct iteration is unnecessary for normal runtime behavior, and a snapshot is more consistent within a user turn.

## Non-Goals

- Do not change provider request formats.
- Do not add Anthropic/OpenAI server-side prompt caching in this step.
- Do not cache active skill rendering; it must remain dynamic per iteration.
- Do not cache deferred tool summaries; discovery state may change mid-turn.
- Do not change memory recall timing; it remains once per user input.
- Do not introduce a large prompt renderer abstraction unless the minimal cache proves insufficient.

## Recommended Design

Cache the rendered default system prompt inside `AgentLoop` and pass it into prompt assembly as a pre-rendered string.

### AgentLoop State

Add a private member to `AgentLoop` in `src/agent/agent-loop.hpp`:

```cpp
std::string default_system_prompt_;
```

`AgentLoop` already stores `prompt::EnvironmentInfo env_info_`. The cache must always match that value.

Add a small private helper:

```cpp
void refresh_default_system_prompt();
```

The helper should assign:

```cpp
default_system_prompt_ = prompt::build_default_system_prompt(env_info_);
```

### Cache Initialization

Initialize `default_system_prompt_` in the `AgentLoop` constructor from the default-constructed `env_info_`.

`AgentLoop::set_environment_info(prompt::EnvironmentInfo info)` is already public and used by the builder/runtime assembly path. It must update both `env_info_` and the cache:

```cpp
void set_environment_info(prompt::EnvironmentInfo info) {
    env_info_ = std::move(info);
    refresh_default_system_prompt();
}
```

`AgentLoopBuilder::build()` should continue to call `loop.set_environment_info(*env_info_)` when configured. This keeps direct construction and builder construction consistent.

### Prompt Assembly Signature

Change `detail::build_system_prompt` from:

```cpp
std::string build_system_prompt(
    const prompt::EnvironmentInfo &env_info,
    std::string_view skills_prompt,
    const ToolRegistry &tools,
    std::string_view memory_section
);
```

to:

```cpp
std::string build_system_prompt(
    std::string_view default_system_prompt,
    std::string_view skills_prompt,
    const ToolRegistry &tools,
    std::string_view memory_section
);
```

The function should keep the same section ordering and metadata:

- `system.default`, `must_keep`, priority `100`
- `system.skills`, `important`, priority `90`
- `system.deferred_tools`, `important`, priority `80`

The only behavior change is that `system.default.content` comes from the cached string instead of invoking `prompt::build_default_system_prompt(...)` per iteration.

### AgentLoop Run Flow

`AgentLoop::run` should keep this ordering:

1. Dispatch `message_received` hook.
2. Append user message and emit history checkpoint.
3. Render memory section once for the user input.
4. For each iteration:
   - refresh active skill section if `skill_loader_ != nullptr`
   - inject incoming messages
   - check stop callback
   - get `tools_->definitions()`
   - build effective system prompt using `default_system_prompt_`, dynamic skills, current deferred tool summaries, and remembered context
   - call provider
   - split response and execute tools as before

This preserves skill activation behavior tested by `run_refreshes_skill_prompt_after_conditional_activation`.

## Trade-Offs

The default prompt will no longer pick up edits to `.orangutan/agent/*.md`, date changes, shell changes, or OS information changes during the lifetime of an existing `AgentLoop` instance unless `set_environment_info(...)` is called. This is an accepted snapshot semantic because those values describe runtime context rather than per-iteration agent state. New runtimes still see updated files and environment values.

If hot reload becomes necessary, add an explicit cache invalidation hook rather than reading files every iteration. A conservative alternative is refreshing `default_system_prompt_` once per user turn before the ReAct loop, but the first implementation should keep the simpler runtime snapshot cache.

## Error Handling

No new error boundary is introduced. `prompt::build_default_system_prompt` currently tolerates missing workspace agent files by treating them as empty. The cache should preserve that behavior.

## Testing

Update or add tests in `tests/agent/agent-loop-test.cpp`.

Required verification:

- `prompt_building_injects_remembered_context_naturally` still passes.
- `run_refreshes_skill_prompt_after_conditional_activation` still passes.
- `run_inserts_continuation_prompt_before_continuation_call` still passes.

Add targeted regression tests:

- Create an `AgentLoop` with workspace agent file contents.
- Run one prompt through a fake provider that records system prompts.
- Mutate the workspace agent file after runtime construction.
- Force a second ReAct iteration using a tool call.
- Verify both provider calls use the original cached workspace agent content.
- Verify the second call still reflects newly activated skills when using the existing conditional skill activation fixture.
- Verify `set_environment_info(...)` refreshes the cached default prompt by changing `workspace_root` or `agent_key` and asserting the next recorded prompt reflects the new environment.

The workspace-file snapshot test and `set_environment_info(...)` refresh test are required because they define the cache semantics. The skill refresh behavior may reuse the existing `run_refreshes_skill_prompt_after_conditional_activation` test if it remains sufficient after the implementation.

Recommended commands:

```bash
xmake build test-agent
xmake run test-agent "prompt_building_injects_remembered_context_naturally"
xmake run test-agent "run_refreshes_skill_prompt_after_conditional_activation"
xmake run test-agent "run_inserts_continuation_prompt_before_continuation_call"
```

## Implementation Notes

- Follow the existing C++23 style: explicit comparisons, no implicit bool checks, no macros, no unnecessary abstractions.
- Keep the edit minimal. This does not need a new `PromptRenderer` class yet.
- Prefer `std::string_view` for read-only prompt inputs.
- Preserve existing prompt section IDs and priorities to avoid changing compiled prompt order.
- Do not change `prompt::compile_prompt` in this step.

## Future Work

After this minimal cache lands, possible follow-ups are:

- Extract a small `PromptBaseCache` if more prompt sections become cacheable.
- Add explicit invalidation when workspace agent files are edited through Orangutan tools.
- Measure provider request construction costs and decide whether `ProviderRequest` should hold views or shared snapshots to reduce history/tool copying.
- Consider provider-specific prompt caching only after the local prompt behavior is stable and covered by tests.
