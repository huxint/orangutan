# Memory

`oran-memory` owns conversation serialization, scoped durable notes, discovery
cues and lexical recall. It does not choose an identity or call a provider.

The library is a functional core over one effect port. Note values, validation,
index selection and rendering are pure functions of their inputs. `Backend` is
the only storage effect; `recall` and `index` sequence those effects without
policy of their own. Bootstrap adapts the result to tools and hooks. Every state
a writer can create must be observable by some reader.
Useful memory closes a loop: discover an applicable lesson, read it, act on it,
correct it when new evidence arrives, and make the correction available later.

| Context | Lifetime and purpose |
| --- | --- |
| Recent conversation | Session-scoped verbatim exchanges for the current task. |
| Memory index | A bounded view of scoped notes, refreshed at each prompt boundary so the agent knows what it can consult. |
| Durable notes | Cross-session preferences, corrections, decisions and references, read in detail when relevant. |

A note should hold one useful lesson. Its title names the topic; its opening
sentence states the fact; the body explains why it matters and when to apply it.
The index derives a cue from existing content, so old records need no rewrite.
Temporary progress stays session-scoped. A derived working-context checkpoint
preserves the active task when older exchanges leave the provider view; it never
becomes a durable note automatically. The [agent contract](agent-platform.md)
owns compaction policy and model requests.

## Consultation And Learning

MemoryRecall and MemoryRemember are active in the default catalogue. The runtime
preamble and tool descriptions tell the agent to consult applicable notes before
choosing an approach or asking the user to repeat context, and to revisit memory
when the task reveals another relevant topic. A durable correction or preference
is saved in the turn where it arises, without waiting for a "remember this"
request. The agent reads an existing related note and reuses its ID when updating
the lesson. One-off instructions, guesses, secrets and facts readily derived from
code do not become durable notes. MemoryForget is also available in the native
tool declarations when the backend is bound, for explicit scoped removal.

These are model behavior instructions. The runtime guarantees the discovery,
read and write paths, but does not infer durability, force a write every turn, or
claim that a successful scripted test proves spontaneous model behavior. Current
owner instructions and current evidence take precedence over saved notes.
Writes use the host-bound scope and memory write gate, without generic tool approval.
Functional tool selection and backend enablement govern availability.

Memory normally changes how the agent does the current work, not what topic it
answers. Consultation is gated by relevance to the current request; unrelated
notes are ignored. Routine reads and writes are not announced, and replies do
not volunteer saved-note contents, user profiles, IDs or "I remember" preambles.
An explicit question about memory, a requested memory change, or a remembered
assumption that must be clarified can warrant a brief, relevant explanation.
Explicit save requests receive confirmation only after successful persistence.
The index identifies itself as background context rather than a user message;
write-field descriptions define reusable knowledge rather than response text.

The distinction between applying and displaying memory follows the
[Claude Code user-memory guidance](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-prompt-user-memory-usage-guidance.md)
and its [internal memory-update framing](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-reminder-memory-updates.md).
These are third-party prompt extractions, not an official behavioral guarantee.
[OpenClaw's tool-call guidance](https://github.com/openclaw/openclaw/blob/main/src/agents/system-prompt.ts)
also keeps routine calls silent. Its
[memory tool contract](https://github.com/openclaw/openclaw/blob/main/extensions/memory-core/src/memory-tool-contract.ts)
separates query/read schemas from recall and citation policy; Orangutan applies
the relevance distinction without adopting automatic memory citations.

This design adopts progressive disclosure from
[Claude Code auto memory](https://code.claude.com/docs/en/memory#auto-memory) and
[Letta MemFS](https://docs.letta.com/concepts/memfs/index.md), and the emphasis on
learning that changes future behavior from Letta's
[Context Constitution](https://github.com/letta-ai/context-constitution/blob/main/constitution/CONSTITUTION.md).
The [memory update trigger prompt](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-memory-write-update-triggers-and-timing.md)
is a third-party extraction used to inform same-turn correction handling.
[LangGraph's memory design](https://docs.langchain.com/oss/python/concepts/memory)
separates thread state from cross-thread knowledge and explains the cost of
foreground versus background learning. The current implementation uses one
foreground write path. OpenHands' [condenser design](https://github.com/OpenHands/software-agent-sdk/blob/main/openhands-sdk/openhands/sdk/context/condenser/README.md)
informs the separation of preserved history from a bounded model view. Orangutan's
session checkpoint and compaction policy are owned by the agent contract.

## Prompt Boundary

AgentSession resolves `config.memory.longterm.recall` automatically. Orientation
is enabled by default when a backend is present, with up to 20 entries and an
8192-byte text budget. An explicit `AgentSessionOptions::longterm_recall` overrides
configuration. Exact caller-supplied `memory_framing` replaces default orientation;
explicitly enabling both is invalid. Disabling automatic orientation leaves
enabled model-directed memory tools available.

Orientation dispatches MemoryRecall with no search query. It uses the ordinary
prepared-call, runtime policy, hook, audit and output-cap boundaries. The returned
index is fixed for the provider/tool iterations in that prompt; an accepted write
appears in the next prompt's index, while its tool result is available immediately
in the current conversation. Lookup times and scores never enter index text.

An empty index says that no notes match the current page/filter. An unavailable,
denied or incomplete index is identified as unavailable; it never masquerades as
empty memory and does not prevent ordinary work. Cancellation still propagates.
A hook rewrite that changes browsing into a full-content read cannot place that
body in the automatic prefix. The host accepts only a complete `memory_index`
output there.

## Session History

`session::Store` wraps `storage::SessionRepository`. Keys are explicit session
and agent values; messages retain typed text, image, thinking, tool-use and
tool-result blocks. Its API appends and reads conversations, with serialization owned by
memory and transactions owned by storage.
Image media types and base64 bytes round-trip in the existing message envelope;
saved text records and database schemas are unchanged.

`load_tail` returns the newest messages in conversation order with row and encoded
byte limits. Defaults are 128 rows and 512 KiB; valid limits are 1–4096 rows and
1–16 MiB. Counting includes content and metadata bytes. Stop at the first row
that does not fit, preserving a contiguous suffix. The full history remains in
SQLite. This remains a bounded browsing API; agent continuation uses forward pages from
its checkpoint so older task context cannot silently disappear.

`Store::append_all` serializes an entire transcript suffix before the repository
acquires its writer. Serialization or storage failure leaves the preceding
conversation intact; success appends every message in order. Single-message
`append` uses the same path. An empty suffix without a checkpoint has no effect. Message encoding and
existing tables are preserved. Individual memory-tool effects commit under their
own dispatch contract and are not rolled back with a failed transcript commit.
Session import maps source and destination session/agent keys explicitly through
the storage repository; it does not import or remap long-term memory scopes.

## Long-Term Memory Recovery

`Fts5Backend::backup_to` captures the whole memory database using the verified
SQLite snapshot operation, including committed WAL data and unrelated tables.
The destination must be a new path; existing backups are never overwritten.
The [storage contract](storage-runtime.md#backup-and-session-import) owns snapshot
mechanics and incomplete-backup handling.

`Fts5Backend::import_scope` requires a source snapshot path and explicit source
and destination scope keys. It opens the source read-only and pins its schema
and records in a read transaction. The supported source schema is version 1.
All records in that scope, including shadow rows, are copied into one empty
destination scope. IDs, linked IDs, kinds, text, timestamps, importance and raw
JSON metadata are preserved; only `scope_key` changes. Linked IDs continue to
refer within the mapped scope, and unresolved links remain unresolved.

The destination transaction streams records and rebuilds their FTS entries from
record content, including tags. It does not trust or copy the source search index.
Any existing destination record or orphan FTS entry causes a conflict, even when
its ID differs from the imported IDs. Missing source scopes, unsupported schemas,
SQL failures and observed cancellation abort without committing records or index
rows. Source records, unrelated scopes, optional tables and migration history
remain untouched. This is whole-scope recovery, not a merge or ID-remapping API.

Both operations are explicit host maintenance APIs, outside the model tool
catalogue. The host supplies authorized scope mappings, owns private and stable
source/destination directories, and runs the operation on the blocking executor.
Snapshot paths remain host-controlled throughout the import. No session, approval,
audit or trace authority is imported. A user-data format migration still requires
a verified backup before applying it; these APIs do not migrate formats.

## Working Context

`Store::load_context` reads a fixed history end and the optional working checkpoint
from reserved `sessions.metadata_json.oran_working_context`. The checkpoint carries
`covered_sequence`, `revision` and the bounded handoff text. Absent checkpoints
start at sequence zero. Invalid metadata, coverage or revisions fail explicitly.
`load_after` reads bounded pages after that sequence without discarding older rows.
Its optional encoded-byte budget defaults to 512 KiB and accepts up to 16 MiB;
an individual row that exceeds the budget fails explicitly rather than being skipped.

`append_all` can accept the loaded snapshot and a provisional checkpoint. It
serializes the whole suffix before acquiring a writer, checks the prior message
count/revision and atomically commits both suffix and checkpoint. Conflicts or
failures leave both unchanged. Checkpoints covering unfinished work are never
persisted independently. Other session metadata fields remain intact, and no new
schema or long-term memory format is introduced.

## Scoped Reads And Records

A `RecordKey` is `(scope_key, id)`. Every read, write and removal supplies that
scope. Record kinds are user, feedback, project, reference and team. These are
stored classifications, not evidence of a running team subsystem.

MemoryRecall has three uses:

| Input | Result |
| --- | --- |
| `{}` or `{"offset":20,"kinds":["feedback"]}` | A compact index page with actionable IDs and cues. Follow `next_offset` with the same filters to continue. |
| `{"id":"response-style"}` | The complete visible note in the host's scope, independent of lexical matching. |
| `{"query":"release workflow","limit":5}` | Full notes matching literal topic words; optional kinds filter the results. |

ID and query are mutually exclusive. Offset applies only to browsing. The tool
limit is 1–20, defaulting to 20 for the index, one for an exact read, and five for
search. JSON cannot select a scope. Ambiguous selectors, blank/control-containing
text, invalid UTF-8 and out-of-range bounds fail before touching the backend.

`Fts5Backend` stores records in `memory.db` and updates its FTS index
transactionally. `Backend::list` returns a page plus one look-ahead cue. SQLite
projects bounded title/body prefixes before copying values to the application.
`make_index` performs pure selection and UTF-8-safe rendering within the byte
budget, keeping entire entries and exact IDs. It clips titles to 120 bytes and
cues to 240 bytes before adding an ellipsis. Feedback comes first, then user
notes, then other kinds; each group sorts by importance, update time and ID.
Read frequency does not affect this order. Pagination assumes unchanged records;
concurrent writes can move page boundaries.

The index text budget is 512–8192 bytes, further constrained by a nonzero tool
text cap. A smaller cap makes automatic orientation unavailable. Oversized legacy
IDs that cannot fit even one entry are reported as omissions, without being
shortened into a different actionable ID. Pagination advances past them. Index
JSON has `kind=memory_index`, entries, `next_offset` and `omitted_count`; it
contains no full record bodies. Browsing never acquires a writer or updates
`last_read_at`.

`recall(Backend&, RecallRequest)` reads an exact ID or searches, updates only the
selected notes' read timestamps, and returns owned content snapshots and
text. All selected reads advance in one write transaction; a note removed
between selection and that write keeps its returned snapshot. Timestamps are
stored at millisecond precision. Ordinary search and exact recall exclude shadow
records, and the model cannot create them; raw backend `get` still inspects
existing rows. Search accepts at most 4096 bytes and 100 backend results.
Up to 32 literal terms are OR-combined, so useful topic terms need not match
every word of a question. ASCII punctuation separates terms; non-ASCII runs use
SQLite's existing unicode61 tokenizer. BM25 weights title/body/tags as 4/1/2.
Ties use update time and ID. This is lexical search: paraphrase, translation and
general Chinese word segmentation are not supplied by this index. Browsing and
exact IDs remain available when wording does not match.

Rendering is a pure function of selected content. Timestamps, request IDs and
scores stay out of prompt text. An existing row's update preserves its
creation and last-read timestamps and keeps update time monotonic; correcting a
lesson replaces that scoped ID and its indexed content instead of duplicating it.
MemoryRemember supplies a complete replacement note rather than a partial update.
Omitted importance uses 0.5; omitted tags and linked IDs become empty lists, so
callers preserving those fields must include them in the replacement. The schema
explains kind selection, discovery priority and these metadata defaults.

Retrieval values, tool-result JSON and memory-read hook hits carry one lexical
`score`.

Memory tools use runtime dispatch with validation, hooks and audit. Generic
allow/deny/ask rules do not apply. Bindings
receive scope from the host. Writes publish a blocking pre-write gate; accepted
writes, reads and removals publish advisory observations. `memory_read_after`
uses source `MemoryRecall` for full records and `MemoryRecall:index` for the
displayed cues. For index observations, hit title/body hold the cue; undisplayed
record metadata uses neutral defaults. The existing redaction boundary applies.

SQLite operations use the blocking executor. Hook publication and session-state
updates resume on the coordinating strand.

User database contents and migration history are preserved during API reduction.
Existing optional index tables, including sqlite-vec data, are left intact when
opening the lexical store. Record metadata, shadow flags and stored kinds retain
their existing encoding.
[Storage](storage-runtime.md) owns persistence mechanics;
[agent execution](agent-platform.md) owns parent/child session composition.

## Behavioral Acceptance

Controlled-provider tests exercise the complete runtime, including a correction
followed by a reopened session, but supply the tool decisions themselves. A
deployment model needs a separate evaluation with ordinary prompts and an
isolated memory scope. Observe actual tool calls and stored rows as well as the
answer; an assertion that the model "remembers" is not persistence evidence.

| Scenario | Expected behavior |
| --- | --- |
| "以后用中文给我结论，理由放到必要的时候再说。" | Save the durable preference in this turn, updating the related note if one exists. |
| New session: "帮我评估这个改动。" | Discover and consult the applicable preference, then apply it without a memory reminder, note recap or "I remember" preamble. |
| "只有这次请用英文写给国外同事。" | Follow this request without replacing the durable language preference. |
| A correction supersedes a saved decision | Read and update the existing ID; subsequent work uses the corrected decision. |
| An unrelated trivial question with saved preferences/projects in the index | Answer the question without unrelated recall, profile summaries or unsupported durable writes. |
| "你记得我对回复格式有什么要求？" | Read the relevant note and answer the memory question directly, without unrelated notes or internal IDs. |
| "记住：以后先给结论。" | Save or update the preference, then briefly confirm the successful save. |
| The memory backend is disabled or a memory write gate refuses a note | Continue with available context; do not claim that a refused write persisted. |

Record consultation success, durable-write success, stale-note use, unwanted
writes, unsolicited memory disclosures and token cost across repeated runs of
the deployment model. Semantic retrieval and background reflection need evidence from these failures before
extending the runtime. Session compaction separately needs deployment-model
measurements of goal retention, constraint loss and summary drift. Pending gates live in
[live debt](../exec-plans/tech-debt-tracker.md).
