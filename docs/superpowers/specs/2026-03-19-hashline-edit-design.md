# Hash-Anchored Edit Mode

**Date:** 2026-03-19
**Status:** Approved

## Overview

Enhance the edit tool with a hash-anchored editing mode inspired by [oh-my-pi](https://github.com/can1357/oh-my-pi). Each line in a file is identified by a `LINE#HASH` tag (e.g., `42#KQ`), allowing the LLM to reference exact line positions without reproducing search text. This reduces token usage by ~20% and improves edit reliability by eliminating ambiguous text matching.

The system operates in dual-mode: both hash-anchored (`hashline`) and the existing search/replace (`search_replace`) modes are available, switchable via config. Default mode is `hashline`.

## Architecture

### New Module: `hashline.hpp/cpp`

Located at `src/features/tools/core/hashline.hpp` and `hashline.cpp`.

#### Hash Algorithm

- **xxHash32** via the [xxHash](https://github.com/Cyan4973/xxHash) library, integrated as a single header with `XXH_INLINE_ALL` defined before including `xxhash.h`. Added via CMake FetchContent from the xxHash GitHub repo; only the `xxhash.h` header is used.
- Truncated to 1 byte: `XXH32(line.data(), line.size(), seed) & 0xFF` (256 buckets)
- Encoded as 2-character string from a 16-character alphabet: `ZPMQVRWSNKTXJBYH`
- Mapping formula: `HASH_DICT[byte] = alphabet[byte >> 4] + alphabet[byte & 0xF]` (high nibble first)
- Precomputed 256-entry lookup table (`HASH_DICT`) maps each byte to its 2-char encoding

#### Seeding Rule

- **Symbol-only lines** (no alphanumeric characters — e.g., `{`, `}`, `()`, empty lines): use 1-indexed line number as hash seed
- **All other lines**: seed = 0

This prevents common symbol-only lines from colliding (they would all hash identically with seed 0).

#### Line Preprocessing

Before hashing: strip `\r`, trim trailing whitespace. This ensures hash stability across minor formatting differences.

#### Staleness Detection

Per-line hash validation is used for staleness detection. There is no whole-file hash — this is a deliberate design choice matching oh-my-pi. Staleness is detected per-anchor: if the file was modified between read and edit, the affected line hashes will not match and the edit will be rejected with corrected hashes. This tolerates partial staleness (some lines changed, others unchanged) and gives precise mismatch information.

#### Key Functions

```cpp
// Compute 2-char hash for a line
std::string compute_line_hash(std::string_view line, size_t line_number);

// Format a line with hash tag: "LINE#HASH:content"
std::string format_hashline(std::string_view line, size_t line_number);

// Parse anchor string "42#KQ" into {line_number, hash}
struct Anchor { size_t line; std::string hash; };
Anchor parse_anchor(std::string_view anchor_str);

// Validate anchor against file contents, return mismatch info if invalid
struct HashMismatch { size_t line; std::string expected; std::string actual; };
std::optional<HashMismatch> validate_anchor(const Anchor& anchor,
                                             const std::vector<std::string>& lines);
```

### Read Tool Changes

File: `src/features/tools/core/read.cpp`

The `register_read_tool` signature changes to accept the edit mode:

```cpp
void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root,
                        std::string_view edit_mode);
```

When `edit_mode == "hashline"`:
- Output each line as `LINE#HASH:content` instead of plain numbered lines
- Line numbers in hashes always refer to **original file line numbers**, not display offsets (so `offset`/`limit` parameters don't affect hash validity)
- **Multi-path reads** (`paths` parameter): each file section still uses the existing `=== path ===` header, and lines within each section are hash-tagged using that file's line numbers

When `edit_mode == "search_replace"`:
- Output unchanged (current behavior)

### Edit Tool Changes

File: `src/features/tools/core/edit.cpp`

The `register_edit_tool` signature changes to accept the edit mode:

```cpp
void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root,
                        std::string_view edit_mode);
```

Based on `edit_mode`, the tool registers with the appropriate schema, description, and execution lambda.

#### Hashline Mode Schema

When `edit_mode == "hashline"`, the edit tool accepts:

```json
{
  "type": "object",
  "properties": {
    "path": { "type": "string", "description": "File path to edit" },
    "edits": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "op": { "type": "string", "enum": ["replace", "insert_after", "insert_before", "delete"] },
          "anchor": { "type": "string", "description": "Line anchor in LINE#HASH format" },
          "end_anchor": { "type": "string", "description": "End anchor for range operations (inclusive)" },
          "content": {
            "oneOf": [
              { "type": "array", "items": { "type": "string" } },
              { "type": "string" }
            ],
            "description": "Replacement/insertion lines. String content is split on newlines."
          }
        },
        "required": ["op"]
      }
    }
  },
  "required": ["path", "edits"]
}
```

**Note on `required` fields:** Only `op` is required at the JSON schema level. Per-operation field requirements (e.g., `anchor` required for `replace` and `delete`, `content` required for `replace` and `insert_*`) are validated at runtime with clear error messages. This keeps the schema simple while allowing proper per-op validation.

**Content type handling:** When `content` is a single string, it is split on `\n` into a `std::vector<std::string>`. When it is already an array, each element is one line.

#### Operations

| Op | Anchor | End Anchor | Content | Effect |
|----|--------|-----------|---------|--------|
| `replace` | required | optional | required (can be `[]`) | Replace single line or range (inclusive) |
| `insert_after` | optional | — | required | Insert after anchor. No anchor = append to EOF |
| `insert_before` | optional | — | required | Insert before anchor. No anchor = prepend to BOF |
| `delete` | required | optional | — | Delete single line or range (inclusive) |

#### Application Algorithm

1. **Read file** into `std::vector<std::string>` by lines
2. **Resolve path** using existing `resolve_tool_path()` with sandbox enforcement
3. **Per-op validation**: Check each edit has the required fields for its `op` type. Reject with a clear error if missing (e.g., `"replace requires 'anchor'"`)
4. **Parse all anchors** from edit operations. **Anchor-less operations** (insert_after/insert_before without anchor) skip anchor parsing and validation.
5. **Validate all anchors**: for each anchor, check line is in range and recompute hash matches. Collect **all** mismatches before failing (atomic pre-validation)
6. **Detect overlapping ranges**: If two edits target overlapping line ranges (e.g., replace 5-10 and delete 8-12), reject with error: `"overlapping edits at lines X-Y and A-B"`. Overlapping is defined as any intersection of the [anchor.line, end_anchor.line] intervals for replace/delete operations.
7. **Deduplicate**: Two edits are "identical" when they have the same `op`, `anchor`, `end_anchor`, and `content`. If two edits target the same anchor(s) with **different** content, this is an error: `"conflicting edits at line N"`
8. **Sort edits bottom-up**: Highest effective line number first. Precedence at same line: replace/delete > insert_after > insert_before. **Anchor-less operations**: EOF appends sort first (applied first = highest position), BOF prepends sort last (applied last = lowest position). Within anchor-less operations of the same type, preserve input order.
9. **Apply edits** via vector operations, working from bottom to top (avoids index shifting):
   - `replace` single: erase line at `anchor.line-1`, insert `content` lines at that position
   - `replace` range: erase from `anchor.line-1` to `end_anchor.line-1` (inclusive), insert `content` at start position
   - `insert_after` with anchor: insert `content` after `anchor.line-1`
   - `insert_after` without anchor: append `content` to end of vector
   - `insert_before` with anchor: insert `content` before `anchor.line-1`
   - `insert_before` without anchor: insert `content` at position 0
   - `delete` single: erase line at `anchor.line-1`
   - `delete` range: erase from `anchor.line-1` to `end_anchor.line-1` (inclusive)
10. **Write file** back to disk
11. **Return summary**: "Applied N edits to path"

#### Error Handling

- **Hash mismatch**: Error message shows 2 lines of context around each mismatched line, with the **correct** `LINE#HASH` so the LLM can update stale references without re-reading:
  ```
  Hash mismatch at line 42: expected KQ, got PM
    40#VR:  int x = 1;
    41#WS:  int y = 2;
  >>> 42#PM:  return x - y;  // <-- actual content
    43#NX:  }
  ```
- **Out-of-range**: `"line N is beyond file length (M lines)"`
- **Invalid range**: `"start anchor line must be <= end anchor line"`
- **Overlapping edits**: `"overlapping edits at lines X-Y and A-B"`
- **Conflicting edits**: `"conflicting edits at line N: same target with different content"`
- **Missing required fields**: `"replace requires 'anchor'"`, `"insert_after requires 'content'"`
- **Content auto-stripping**: If **all** non-empty replacement lines match the pattern `^\d+#[ZPMQVRWSNKTXJBYH]{2}:(.*)`, auto-strip the prefix and include a warning in the result: `"Warning: stripped hashline prefixes from content"`
- **Noop detection**: If a replace would produce identical content, warn and skip: `"Noop: line N already has the specified content"`

#### Hashline Mode Tool Description

The `ToolDef::description` for hashline mode:

```
Edit a file using hash-anchored line references. Lines are identified by LINE#HASH tags
from the read tool output (e.g., "42#KQ"). Provide a path and an array of edit operations.

Operations:
- replace: Replace line(s) at anchor (single) or anchor..end_anchor (range) with content
- insert_after: Insert content after anchor line (omit anchor to append to EOF)
- insert_before: Insert content before anchor line (omit anchor to prepend to BOF)
- delete: Delete line at anchor (single) or anchor..end_anchor (range)

If a hash doesn't match (file changed since read), the error shows the correct hashes.
```

### Search/Replace Mode

When `edit_mode == "search_replace"`, the edit tool continues to use the existing patch format and all current behavior is preserved unchanged. The tool description and schema remain as-is.

### Config Integration

File: `src/infra/config/config.hpp` / `config.cpp`

New field in the `Config` struct, in the `[tools]` section:

```cpp
// [tools] section
std::vector<std::string> allowed_tools;
std::vector<std::string> denied_tools;
std::string edit_mode = "hashline";  // "hashline" | "search_replace"
```

Parsed in `parse_tools_section()` within `config.cpp`:

```cpp
if (auto mode = tools_table["edit_mode"].value<std::string>()) {
    config.edit_mode = *mode;
}
```

TOML representation:
```toml
[tools]
edit_mode = "hashline"  # "hashline" | "search_replace", default: "hashline"
```

- CLI override: `--edit-mode=hashline|search_replace` (parsed in `main.cpp` alongside existing CLI flags)
- The config value is passed through to `register_builtin_core_tools()` which forwards it to `register_edit_tool()` and `register_read_tool()`

### Registration Signature Changes

File: `src/features/tools/core/internal.hpp`

Updated signatures:

```cpp
void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root,
                        std::string_view edit_mode);
void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root,
                        std::string_view edit_mode);
```

File: `src/features/tools/core/register-core.cpp`

The `register_builtin_core_tools` function receives `edit_mode` from the config and passes it to both tool registration functions.

### Dependencies

- **xxHash**: Added via CMake FetchContent from `https://github.com/Cyan4973/xxHash.git` (tag v0.8.3). Only the `xxhash.h` header is used with `#define XXH_INLINE_ALL` before `#include "xxhash.h"`. This makes it fully inline / header-only with no library linking needed. MIT license.

### Files to Create/Modify

| File | Action |
|------|--------|
| `src/features/tools/core/hashline.hpp` | Create — hash computation, formatting, parsing, validation, edit application |
| `src/features/tools/core/hashline.cpp` | Create — implementation |
| `src/features/tools/core/edit.cpp` | Modify — add hashline execution path alongside existing search/replace |
| `src/features/tools/core/read.cpp` | Modify — conditional hash-tagged output |
| `src/features/tools/core/internal.hpp` | Modify — update `register_read_tool` and `register_edit_tool` signatures |
| `src/features/tools/core/register-core.cpp` | Modify — pass edit_mode to tool registration |
| `src/infra/config/config.hpp` | Modify — add `edit_mode` field to `Config` struct |
| `src/infra/config/config.cpp` | Modify — parse `edit_mode` in `parse_tools_section()` |
| `src/app/main.cpp` | Modify — add `--edit-mode` CLI flag |
| `CMakeLists.txt` | Modify — add xxHash FetchContent, add `hashline.cpp` to sources |
| `tests/core/tool-registry-test.cpp` | Modify — add hashline edit mode integration tests |
| `tests/features/hashline-test.cpp` | Create — unit tests for hash computation, parsing, validation, application |

### Test Plan

1. **Hash computation**: Known inputs produce expected 2-char hashes; symbol-only lines with different positions produce different hashes; identical content lines produce same hash
2. **Anchor parsing**: Valid `"42#KQ"` formats parse correctly; invalid formats (missing `#`, bad hash chars, non-numeric line) rejected
3. **Anchor validation**: Correct hash passes; wrong hash returns mismatch with actual hash; out-of-range rejected
4. **Edit operations**: Replace single/range, insert_after/before with/without anchor, delete single/range
5. **Bottom-up application**: Multiple edits at different positions apply correctly without index confusion
6. **Atomic validation**: If any anchor is invalid, no edits are applied
7. **Overlapping range detection**: Overlapping edits rejected before application
8. **Conflicting edit detection**: Same target with different content rejected
9. **Content auto-stripping**: Hash prefixes in replacement content are detected and stripped
10. **Content string splitting**: Single string content correctly split on newlines
11. **Noop detection**: Replace with identical content is flagged
12. **Mode switching**: Correct schema and behavior in each mode
13. **Read tool**: Hash-tagged output in hashline mode (single and multi-path), plain in search_replace mode
14. **Integration**: End-to-end edit via tool registry in both modes
15. **Config parsing**: `edit_mode` read from TOML and CLI override
