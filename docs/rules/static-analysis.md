# Static Analysis

GCC 16.1 ships a mature `-fanalyzer` and a wide set of `-Wanalyzer-*` warnings. This
rule defines **when** the analyzer runs, **what** it flags as a hard failure, and
**how** to suppress a false positive without weakening the safety net for the rest of
the codebase.

## Modes

| Mode               | xmake invocation                            | When           | Warnings → errors? |
| ------------------ | -------------------------------------------- | -------------- | ------------------ |
| Off (default)      | `xmake f -m release`                         | Local devel.   | n/a                |
| Analyze (manual)   | `xmake f -m release --analyze=y`             | Author audit.  | yes                |
| CI Hardening       | `xmake f -m release --analyze=y --hardened=y`| Nightly CI.    | yes                |

The `--analyze=y` option enables `-fanalyzer` and the analyzer-specific warning
escalation. `--hardened=y` (already documented in
[`../BUILD_SYSTEM.md`](../BUILD_SYSTEM.md)) layers on `_FORTIFY_SOURCE=3`,
`-fstack-protector-strong`, `-fcf-protection`, `-fstack-clash-protection`.

Hosted analyzer coverage remains open in
[live debt](../exec-plans/tech-debt-tracker.md). Prioritize descriptor ownership
and byte parsing in IO, storage, configuration and provider code.

## Required Warnings (Hard Failures)

The analyzer escalates these to errors via `-Werror=` when `--analyze=y` is on:

- `-Wanalyzer-null-dereference`
- `-Wanalyzer-use-after-free`
- `-Wanalyzer-double-free`
- `-Wanalyzer-malloc-leak`
- `-Wanalyzer-tainted-allocation-size`
- `-Wanalyzer-tainted-array-index`
- `-Wanalyzer-out-of-bounds`
- `-Wanalyzer-write-to-string-literal`
- `-Wanalyzer-fd-leak`
- `-Wanalyzer-fd-use-without-check`

The full set is in `xmake/build-policy.lua`'s `oran.build` rule. Add a warning to the
list only with rationale in the PR (and in `docs/exec-plans/tech-debt-tracker.md` if
the addition would be retroactively painful).

## Suppression Rules

- **No file-wide pragmas.** They hide real bugs in code that didn't trigger the
  initial false positive.
- **Function-local pragma is the unit of suppression**:
  ```cpp
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
  // narrow scope: only the call below confuses the analyzer (see explanation).
  Bar* b = third_party_take_ownership(make());
  #pragma GCC diagnostic pop
  ```
- The pragma **must** be preceded by a one-line comment explaining why the
  analyzer is wrong here (an external-ownership transfer, a known GCC bug with a link,
  …). The comment is the contract the next agent reads.
- Reviewers reject suppressions without that comment.

## When To Add Analyzer Coverage

- Any TU that handles raw memory (smart pointers don't need it; `make_unique` paths
  are already safe), file descriptors, sockets, or libc handles.
- Any TU that parses untrusted bytes (configuration and provider payloads).
- Any future TU that implements subprocess plumbing.

The hosted job must identify covered and exempt TUs, with reasons for exemptions.

## Performance Implications

`-fanalyzer` adds **roughly 2-3×** to a TU's compile time on the heaviest
`oran-agent` / `oran-provider` files. That breaks the compile budget in
[`compile-budget.md`](compile-budget.md) if it ran on every TU on every build. The
two-step workflow (default off, opt-in for audits and nightly CI) is the documented
compromise.

If the analyzer + the project's own compile budget come into conflict for a
particular TU, the **right** fix is to split the TU (per
[`compile-budget.md`](compile-budget.md) "When You Hit The Budget") rather than to
disable the analyzer.

## Interaction With clang-tidy

- clang-tidy is required by [`critical-rules.md#C9`](critical-rules.md), but the
  hosted CI job is not provisioned yet. The activation gap is tracked in
  [live debt](../exec-plans/tech-debt-tracker.md); local/editor findings do
  not substitute for the missing gate.
- The analyzer covers a different shape of bug (path-sensitive dataflow). Both are
  required; one does not substitute for the other.
- Where clang-tidy and `-fanalyzer` overlap (e.g., null-deref), the analyzer's
  verdict wins because it is path-sensitive.

## Enforcement

- Configure with `xmake f --analyze=y`, then run the affected build targets.
  Compilation fails if any required warning fires. The shared project policy
  applies the flags; [BUILD_SYSTEM](../BUILD_SYSTEM.md) owns option behavior.
- The TU coverage inventory and hosted analyzer job remain tracked debt.

## See Also

- [`critical-rules.md`](critical-rules.md) — analyzer rule line.
- [`compile-budget.md`](compile-budget.md) — why analyzer is opt-in.
- [`../BUILD_SYSTEM.md`](../BUILD_SYSTEM.md) — `--analyze=y` and `--hardened=y`
  invocation.
- [GCC 16.1 analyzer manual](https://gcc.gnu.org/onlinedocs/gcc/Static-Analyzer-Options.html)
