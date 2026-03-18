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

- **xxHash32** via the [xxHash](https://github.com/Cyan4973/xxHash) library (single-header, added via FetchContent)
- Truncated to 1 byte: `xxHash32(line, seed) & 0xFF` (256 buckets)
- Encoded as 2-character string from a 16-character alphabet: `ZPMQVRWSNKTXJBYH`
- Precomputed 256-entry lookup table (`HASH_DICT`) maps each byte to its 2-char encoding

#### Seeding Rule

- **Symbol-only lines** (no alphanumeric characters — e.g., `{`, `}`, `()`, empty lines): use 1-indexed line number as hash seed
- **All other lines**: seed = 0

This prevents common symbol-only lines from colliding (they would all hash identically with seed 0).

#### Line Preprocessing

Before hashing: strip `\r`, trim trailing whitespace. This ensures hash stability across minor formatting differences.

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
std::optional<HashMismatch> validate_anchor(const Anchor& anchor, const std::vector<std::string>& lines);
```

### Read Tool Changes

File: `src/features/tools/core/read.cpp`

When `edit_mode == "hashline"`:
- Output each line as `LINE#HASH:content` instead of plain numbered lines
- Line numbers in hashes always refer to **original file line numbers**, not display offsets (so `offset`/`limit` parameters don't affect hash validity)

When `edit_mode == "search_replace"`:
- Output unchanged (current behavior)

### Edit Tool Changes

File: `src/features/tools/core/edit.cpp`

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
            "description": "Replacement/insertion content as lines array or single string"
          }
        },
        "required": ["op"]
      }
    }
  },
  "required": ["path", "edits"]
}
```

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
3. **Parse all anchors** from edit operations
4. **Validate all anchors**: for each anchor, check line is in range and recompute hash matches. Collect **all** mismatches before failing (atomic pre-validation)
5. **Deduplicate** identical edits targeting the same anchor(s)
6. **Sort edits bottom-up**: highest line number first. Precedence at same line: replace/delete > insert_after > insert_before
7. **Apply edits** via vector splice operations, working from bottom to top (avoids index shifting)
8. **Write file** back to disk
9. **Return summary**: "Applied N edits to path"

#### Error Handling

- **Hash mismatch**: Error message shows 2 lines of context around each mismatched line, with the **correct** `LINE#HASH` so the LLM can update stale references:
  ```
  Hash mismatch at line 42: expected KQ, got PM
    40#VR:  int x = 1;
    41#WS:  int y = 2;
  >>> 42#PM:  return x - y;  // <-- actual content
    43#NX:  }
  ```
- **Out-of-range**: `"line N is beyond file length (M lines)"`
- **Invalid range**: `"start anchor line must be <= end anchor line"`
- **Content auto-stripping**: If replacement lines contain `LINE#HASH:` prefixes (LLM accidentally copied from read output), auto-strip them and include a warning in the result
- **Noop detection**: If a replace would produce identical content, warn and skip

### Search/Replace Mode

When `edit_mode == "search_replace"`, the edit tool continues to use the existing patch format and all current behavior is preserved unchanged.

### Config Integration

File: `src/core/config.hpp` / `config.cpp`

New field in config:
```toml
[tools]
edit_mode = "hashline"  # "hashline" | "search_replace", default: "hashline"
```

- CLI override: `--edit-mode=hashline|search_replace`
- The edit tool dynamically selects its JSON schema and execution path based on this config
- The read tool checks this config to decide output format

### System Prompt

The tool description sent to the LLM adapts based on mode. In hashline mode:
- Explains that file lines are tagged with `LINE#HASH` in read output
- Documents the JSON edit operations and anchor format
- Notes that stale hashes will be rejected with corrected values

### Dependencies

- **xxHash**: Added via CMake FetchContent (header-only mode). MIT license. ~15KB single header.

### Files to Create/Modify

| File | Action |
|------|--------|
| `src/features/tools/core/hashline.hpp` | Create — hash computation, formatting, parsing, validation |
| `src/features/tools/core/hashline.cpp` | Create — implementation |
| `src/features/tools/core/edit.cpp` | Modify — add hashline execution path alongside existing search/replace |
| `src/features/tools/core/read.cpp` | Modify — conditional hash-tagged output |
| `src/features/tools/core/register-core.cpp` | Modify — pass edit mode to registration |
| `src/core/config.hpp` | Modify — add `edit_mode` field |
| `src/core/config.cpp` | Modify — parse `edit_mode` from TOML and CLI |
| `CMakeLists.txt` | Modify — add xxHash FetchContent, add hashline.cpp to sources |
| `tests/core/hashline-test.cpp` | Create — unit tests for hash computation, parsing, validation, application |
| `tests/core/tool-registry-test.cpp` | Modify — add hashline edit mode integration tests |

### Test Plan

1. **Hash computation**: Known inputs produce expected 2-char hashes; symbol-only lines with different positions produce different hashes; identical content lines produce same hash
2. **Anchor parsing**: Valid formats parse correctly; invalid formats rejected
3. **Anchor validation**: Correct hash passes; wrong hash returns mismatch; out-of-range rejected
4. **Edit operations**: Replace single/range, insert_after/before with/without anchor, delete single/range
5. **Bottom-up application**: Multiple edits at different positions apply correctly without index confusion
6. **Atomic validation**: If any anchor is invalid, no edits are applied
7. **Content auto-stripping**: Hash prefixes in replacement content are detected and stripped
8. **Noop detection**: Replace with identical content is flagged
9. **Mode switching**: Correct schema and behavior in each mode
10. **Read tool**: Hash-tagged output in hashline mode, plain in search_replace mode
11. **Integration**: End-to-end edit via tool registry in both modes
