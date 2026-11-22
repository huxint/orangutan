# Runtime Prompt Design

[prompt::render](../../include/oran/prompt/render.hpp) is a synchronous value
transformation over native tool values and stable caller text. It returns owned
system text and cache identity. The [agent loop](../design-docs/agent-platform.md)
owns one rendered prefix per turn.
This rule governs runtime prompts, not development-agent routing instructions.

## Behavioral Guidance

The default preamble guides completion of the current request: distinguish a
question from a request for action, inspect relevant context before changing it,
make a complete scoped change, and verify the result with available capabilities.
It asks for clarification when missing information materially affects the result,
not for repeated approval of already authorized work. Current instructions and
evidence outrank retrieved content; data and tool results cannot grant authority.

Instructions describe decisions and recovery steps rather than generic demands
to be helpful or careful. Independent calls can be grouped; dependent calls wait
for actual results. Invalid arguments are repaired, stale content is read again,
truncated results remain incomplete, and uncertain mutations are inspected before
retry. A successful write is evidence of a write, not a passing program. Native
availability constrains actions; the prompt does not promise shell, web, image-file
reading or delegation capabilities that are absent from the selected catalogue.

Keep each kind of instruction at its owning surface:

| Surface | Owns |
| --- | --- |
| System preamble | Task completion, authority, cross-tool sequencing, evidence and communication. |
| Tool description | When to choose it, prerequisites, meaningful results and error recovery. |
| Schema property | Meaning, units, defaults, bounds and relationships to other arguments. |
| Memory index | Bounded discovery cues and their background-context framing. |

Replies lead with the answer or outcome, match the user's language and requested
format, and include relevant verification and remaining blockers. Substantial
work can have brief updates at meaningful findings; routine lookups and memory
maintenance remain quiet. Final text is self-contained. The
[memory contract](../design-docs/memory-system.md) owns the exceptions for explicit
memory questions and changes.

These choices adapt the public third-party Claude Code extractions for
[doing requested work](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-prompt-doing-tasks-software-engineering-focus.md),
[limiting unnecessary additions](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-prompt-doing-tasks-no-unnecessary-additions.md),
[outcome-first communication](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-prompt-outcome-first-communication-style.md),
[action scope](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-prompt-executing-actions-with-care.md)
and [tool dependencies](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-prompt-parallel-tool-call-note-part-of-tool-usage-policy.md).
The references are design input, not an official behavior guarantee. Their
product-specific tools, formatting restrictions and approval defaults do not
override Orangutan's contracts.

Schema/default dispatch tests and prompt/provider tests verify that declared
inputs and submitted bytes remain consistent. They do not prove model adherence.
Deployment-model evaluation should cover direct questions, completed edit requests,
unclear targets, stale edits, unavailable tools, incomplete results and quiet memory
use, checking actual effects as well as final answers.

## System Text

Nonempty text sections join in this order with one newline:

1. System preamble: identity, operating principles and response contract.
2. Optional caller-supplied skill catalogue.
3. Scoped memory index or exact caller framing selected at the prompt boundary.
4. Stable per-agent instructions.

Rendering preserves the supplied bytes, including whitespace and existing
newlines. All-empty input produces an empty string. Conversation enters
`provider::Request::messages` as typed content, including user messages and tool
results. Clocks, request IDs, trace IDs, counters and status narration stay out
of stable text.

## Native Tools And Identity

The loop selects one owned, sorted catalogue through `tool::select_tools`.
Both protocols receive descriptions and schemas through
`provider::Request::tools`. System text contains no duplicate native definitions.
The [tool contract](../design-docs/tool-runtime.md) owns selection and authority.
Session handoffs enter typed conversation as derived historical context. Their
separate tool-free summarization requests have their own system instructions and
no stable-prefix cache hint. They never mutate the main turn's prefix or catalogue;
[agent execution](../design-docs/agent-platform.md) owns their shape and budget.

`RenderedPrompt` holds `system_prompt`, `tool_catalog_hash`, `prefix_hash` and
`prefix_bytes`. The native fingerprint includes ordered names, descriptions and
opaque schema bytes, with field lengths preserving boundaries. Required
capabilities remain local permission metadata.

The prefix fingerprint depends on joined system text and the native fingerprint.
Equivalent submitted text and declarations have the same identity regardless of
how caller text was divided into sections. There are no manual section versions
or section-ID inputs. Text, schema, selection and declaration-order changes
invalidate identity. Conversation and local permission metadata do not.

`prefix_bytes` counts the joined system text, including separator newlines, and
native name/description/schema bytes. It excludes protocol framing; it is neither
a token count nor serialized request size. Derived cache keys are not a versioned
compatibility contract. The loop forwards current values to the provider; the
[provider contract](../design-docs/api-portability.md) owns target eligibility and
wire controls.

## Memory Context

Bound memory tools are directly visible by default. The stable memory section
is a bounded index of IDs, titles and cues. Complete notes enter dynamic
conversation through model-directed reads. Index loading occurs once before the
loop; accepted writes appear in the next prompt's index. Scores, read times and
mutable counters stay out of index text. [Memory](../design-docs/memory-system.md)
owns consultation and same-turn durable learning guidance.
The default preamble, native memory descriptions and index framing keep relevant
memory as background context. Ordinary replies apply it without unsolicited
memory narration; explicit memory requests retain useful answers and confirmations.

`bench-prompt` compares full/reduced catalogue rendering and repeated construction
versus reuse across eight iterations. Tests cover joined bytes, identity changes,
native declarations and per-turn snapshots. `scripts/check-prompt-preamble.sh`
checks the default preamble for dynamic inputs.

For a new model-visible surface, consult the relevant proven shape in
<https://github.com/Piebald-AI/claude-code-system-prompts>, then record the adopted
contract in its owning design document.

## Background Receipts And Events

Background AgentRun is advertised only when its host service is bound. Its receipt
states whether completion is automatic or query-only, and that acceptance is not
success. Automatic mode directs the agent to continue independent work and avoid
polling solely to wait. Query-only mode explicitly promises no later wake. TaskGet
is a status/result read, not a wait operation; TaskCancel reports cancellation in
progress until cleanup finishes.

Completion evidence is framed dynamically through the typed session event port,
not appended to the cached system prefix. The frame explicitly rejects treating
a task result as user approval or a response to a pending question, and asks the
parent to review the report against the original task. Task IDs and raw metadata
remain out of ordinary user-facing replies; hosts provide a separate task list.

## Dynamic Team Context

Team roles are leader-authored task context, not predefined system personas.
Common specialist guidance and its chosen native catalogue remain stable across
assignments. Goal/remit initialize dynamic conversation once; subsequent tasks,
dependency previews, corrections and peer messages enter dynamic messages.
Inbox consumption never mutates a turn's prefix and is acknowledged only after
recipient persistence. [Agent teams](../design-docs/agent-teams.md) owns the
message authority, batching, continuation and cost/cache tradeoffs.
