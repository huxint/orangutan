# Investigation Methodology

When an agent is uncertain how to implement a feature, how to fix a tricky
bug, or whether a library shape matches the project's needs, the answer is
**not** to start typing code and iterate against the compiler. This file is
the rule for the *research before code* phase.

> **Why this rule exists.** Two failure modes have been observed in the
> legacy `orangutan/` codebase and in early v2 work: (1) the agent spawns
> half a dozen subagents to "explore alternatives" and then has no signal
> to pick between their answers; (2) the agent copies a snippet from a
> web search wholesale, and the snippet's assumptions (older C++ standard,
> different async model, different error type) silently mismatch the
> project. Both waste tokens and produce diffs that fail review.

## When To Investigate Before Coding

- The design doc / spec for the area is silent on the decision you're
  about to make.
- More than one viable implementation shape exists and you cannot
  immediately rank them.
- A library you intend to use has a recent API break and the version
  pinned in `xmake/packages.lua` may not match the docs you remember.
- An approach you tried failed in a way that does not match the
  exception message — i.e. the failure mode itself is unfamiliar.

Routine, well-trodden work (adding a test case, wiring a new flag through
an existing parser, mirroring an existing repository's shape) does **not**
warrant an investigation pass — just do it.

## Subagent / Teammate Fan-out: Cap At Two

When you do investigate, **at most two** subagents or teammates may be
spawned in parallel for the same question. The shapes that satisfy this
budget:

- **Two contrasting probes.** "Subagent A: implement using asio
  coroutines + `Result<T>`; subagent B: implement using a callback +
  asio executor." Returns two alternatives; you pick.
- **One probe + one review.** Subagent A drafts; subagent B reviews A's
  draft against rule files / existing code. Useful when correctness, not
  taste, is the open question.
- **One probe only.** When the question is narrow ("does X compile under
  C++26 GCC 16.1?"), one is enough.

Forbidden shapes:

- **Fan-out > 2.** Spawning three or more subagents on the same question
  produces a "vote" that has no quorum rule and no tiebreaker. Pick
  before launching.
- **Sequential subagent chain that fakes a council.** Spawning A, reading
  the result, then spawning B, then C, etc. is the same anti-pattern as
  fan-out > 2 — same token cost, same lack of decision signal.
- **Subagent for routine work.** Don't delegate "add a test case for
  this branch" — the subagent has no more context than you do and the
  overhead is pure loss.

After the subagent(s) return, **pick one and execute**. Record durable choices
in the matching current design/spec/rule; the rejected alternative
gets a one-line "tried, rejected because …" so the next agent does not
re-litigate.

## Web Search And Context7 — Reference, Not Source

External research (`WebSearch`, `WebFetch`, `Context7`,
`<https://github.com/Piebald-AI/claude-code-system-prompts>` and similar)
is allowed and often *necessary* when the project's design docs are
silent and your training cutoff is too old. The discipline:

- **Search for prior art, not for code to paste.** Read 2–3 sources on
  how a similar problem has been solved; then translate the *idea* into
  the project's idiom (C++26, asio coroutines, `core::Result<T>`, no
  `std::thread`).
- **Never blind-copy.** A snippet that compiles in someone's tutorial may
  use `boost::asio` instead of standalone asio; may throw exceptions
  instead of returning `Result`; may pull a header (`<iostream>`) that
  this project bans. Always re-shape to project rules before pasting.
- **Pick one or two ideas, not the whole approach.** External work is
  rarely an exact match. Identify the load-bearing insight (e.g.
  "use a strand to serialize access to the registry"), apply it,
  drop the rest.
- **Cite the source in the owning design doc.** When a non-obvious shape
  comes from prior art, record where it came from when the source remains
  relevant to the current contract. Future agents and
  reviewers should be able to reproduce the reasoning.

For library docs specifically, prefer **`Context7`** over `WebSearch`:
it pulls the current docs of the library, not a search-engine-ranked
mix of forum posts and stale tutorials. `WebSearch` is right for
conceptual prior art ("how do others structure a ReAct loop?"),
`Context7` is right for syntax / API ("what's the current `asio::co_spawn`
signature?").

## When An Approach Fails: Check For Stale Knowledge

This is the single most expensive failure mode for an LLM agent — the
training data was correct *at the time* but the library / language /
tooling has since changed. Symptoms:

- The compiler rejects code you "know" should compile.
- A flag / function / class name produces "not found" or "undefined".
- The behavior diverges from the docstring you remember.
- A search-engine snippet from 2023 disagrees with the current header.

Before retrying the same approach with minor tweaks, **stop and
refresh**:

1. Check `xmake/packages.lua` and `docs/rules/libraries.md` for the requested
   version of the library in question. Your "knowledge" of the API may be from
   a different major version.
2. Use `Context7` to fetch the current docs for that pinned version.
3. If `Context7` does not cover it, fall back to `WebFetch` against
   the library's own docs site / changelog. Forum posts are a last
   resort.
4. If the language / compiler is the suspect, check
   <https://gcc.gnu.org/projects/cxx-status.html> and the GCC 16.1
   release notes for the feature.
5. Only after refreshing do you decide whether the original approach
   was right and you typo'd, or whether the approach is dead and a
   different one is needed.

Repeating the same failing approach without refreshing is a budget
leak — each retry burns tokens against a hypothesis you have not
re-grounded.

## What To Record

Investigation that produces a durable decision belongs in the current
design/spec/rule that owns it: which alternatives were considered, which was
picked, and why. Investigation that produces a rule (e.g. "do not use library
X because Y") belongs in the matching rule file under `docs/rules/`.
Investigation that produces a follow-up task belongs in
`docs/exec-plans/tech-debt-tracker.md`. Investigation that produces
nothing reusable can be discarded — not every research pass turns into a
durable artifact.

## Anti-Patterns

- Spawning subagents because "exploration is virtuous". It is not free.
  Spawn when the question genuinely has more than one viable answer.
- Treating a subagent's verdict as ground truth. Subagents have the same
  knowledge cutoff and similar biases; their answers are *hypotheses to
  rank*, not gospel.
- Searching the web and then implementing the first hit. The first hit
  is rarely the best fit; it is the most SEO-optimized.
- Skipping the refresh step on a failing approach because "I'm sure of
  the API". If you were sure, the approach would not be failing.
- Burying the investigation in chat without writing the decision down.
  The next agent has no chat memory — if the decision is not in
  `docs/`, it did not happen.

## Enforcement

No mechanical check today — this is a review-time rule. PR reviewers
flag investigation deficits when:

- A design decision says "tried X, didn't work, used Y"
  without naming what was tried or why X failed.
- A subagent fan-out > 2 appears in the agent's transcript and the
  decision was made by "voting".
- A code change copies an external shape that violates a project rule
  (raw `std::thread`, exception-throwing API, `<iostream>` in
  `src/oran-*/`).

## See Also

- [`prompt-design.md`](prompt-design.md) — references the
  `claude-code-system-prompts` corpus as a study reference; the
  *reference, not source* principle is the same.
- [`libraries.md`](libraries.md) — the canonical approved-library list;
  external research must end with a choice that's already on this list
  or proposes adding to it.
- [`docs-in-sync.md`](docs-in-sync.md) — the Prime Directive: research
  outcomes that change behavior require docs in the same PR.
