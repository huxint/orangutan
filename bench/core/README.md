# `bench/core/` — nanobench scenarios for `oran-core`

## What this bucket benchmarks

`oran-core` is the project's foundation: `Error`, `Result<T>`, `Time`,
the ISO-8601 UTC format/parse helpers, the `Role`/`StopReason` enums, the
`Content` variant, `Message`, `ToolDef`, the `core::str` UTF-8 helpers, and
the `Capability` vocabulary. The bench bucket exists for parity
([`docs/rules/critical-rules.md#C12`](../../docs/rules/critical-rules.md))
and to anchor the A-vs-B convention even on a library where performance is not the
primary axis.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/error_construct.cpp`](scenarios/error_construct.cpp) | Fluent builder + `.with` chain *vs.* move-only constructor + sequential `.with` calls. Both produce the identical `Error` value; we measure whether the builder-return-value path costs more or less than the explicit local-object path. |
| [`scenarios/time.cpp`](scenarios/time.cpp) | `core::time::format_iso8601_utc` (explicit `{:04}-...` template) *vs.* `std::format("{:%FT%T}Z", floor<ms>(tp))` (chrono format specifiers). Both render the canonical wire format; the comparison documents the cost of the project's deterministic-wire-format guarantee. The same file also benches `core::time::parse_iso8601_utc` so downstream callers can see the parse-vs-format budget when they round-trip timestamps. |
| [`scenarios/message.cpp`](scenarios/message.cpp) | `std::visit(Overloaded{...}, content)` *vs.* `std::get_if<TextContent>(&content)` over a 32-block mixed-alternative `Message`. Both walks add up text length and ignore non-text blocks; the comparison documents the cost of the project-preferred visitor style against a single-alternative shortcut. A `core.message_walk_blocks` scenario reports the same visit-based walk as the "render this turn" baseline. |
| [`scenarios/tool_def.cpp`](scenarios/tool_def.cpp) | Aggregate-init `ToolDef{...}` with an inline empty schema *vs.* `ToolDef::with_no_input(name, desc)` helper. Both produce the same value; the comparison documents the cost of the helper path (one extra `std::string` move and a function-call frame) so fixture callers can pick with eyes open. |
| [`scenarios/str_utf8.cpp`](scenarios/str_utf8.cpp) | `core::str::is_valid_utf8` strict RFC-3629 walk over a 1024-byte mixed (ASCII + 2/3/4-byte) fixture *vs.* `std::ranges::all_of` ASCII-only short-circuit on the same fixture. The strict walk inspects every byte; the ASCII filter bails on the first non-ASCII byte. Documents the upper-bound cost of the validator over realistic content so callers can decide when to validate vs. take an ASCII fast path first. |
| [`scenarios/capability.cpp`](scenarios/capability.cpp) | Reflection-backed `core::parse_enum<Capability>` *vs.* `std::unordered_map<std::string_view, Capability>::contains`, using the same batch of valid and invalid spellings. |

## Running

```sh
xmake build bench-core
xmake run bench-core
```

Output is nanobench's Markdown table on stdout. Compare measurements under the
same compiler, mode and hardware as described in the testing rule.

## See Also

- [`docs/rules/testing-and-bench.md`](../../docs/rules/testing-and-bench.md)
