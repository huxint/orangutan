# 04 · Content Blocks

Every unit of content in the conversation is a **block**. Messages compose blocks. Blocks stack vertically with 16px between them. Each block type has one visual treatment — consistent across user and agent messages unless noted.

The design craft of this app lives in getting each block right. **When in doubt, test a block in isolation on an empty page before integrating.**

---

## Block catalog

| # | Block | Origin | When |
|---|-------|--------|------|
| 1 | **Text** | user / agent | default prose |
| 2 | **Code** | user / agent | fenced code, with language |
| 3 | **Quote** | user / agent | reply/quote of an earlier block |
| 4 | **Thinking** | agent | explicit thinking trace |
| 5 | **Tool** | agent | a tool was invoked mid-turn |
| 6 | **Delegation** | agent | spawned a worker or messaged a teammate |
| 7 | **Approval** | agent | permission required |
| 8 | **Memory citation** | agent | grounded by retrieved memories |
| 9 | **Image** | user / agent | image content |
| 10 | **Attachment** | user / agent | file shared |
| 11 | **Link preview** | user / agent | URL unfurled |
| 12 | **Table** | user / agent | tabular data |
| 13 | **Diff** | agent | file edit (specialization of tool/code) |
| 14 | **Callout** | agent | info / warn / error annotation |
| 15 | **Working stream** | agent | live-working phrases (ephemeral) |

Blocks 7, 15 are ephemeral during a turn and collapse on completion.

---

## 1 · Text block

Default prose. Markdown supported: paragraphs, headings (h2 / h3 only, no h1 in conversation), emphasis, strong, inline code, links, bulleted + numbered lists, horizontal rule, blockquote.

- **Type:** `body` (16 / 1.60)
- **Container:** none. No border, no background.
- **Headings inside prose:** h2 `h-section` 15/1.35 weight 520; h3 `ui` 14 weight 520. 8px margin above headings, 4px below.
- **Lists:** 20px left padding; `•` or `1.` markers in `text-muted`; 4px between items.
- **Inline code:** `mono-small` (12.5 JetBrains), `surface-raised` background, 3px horizontal padding, 2px radius.
- **Links:** 1px underline in `text-muted`, becomes copper on hover with full opacity underline.
- **Selection:** default browser selection; copying a selection copies as markdown.

---

## 2 · Code block

Fenced code with a language label and copy affordance.

```
┌── typescript ─────────────────────────────  ⎘  ⤢  ─┐
│                                                    │
│   const x = await load(file, {                     │
│     encoding: 'utf-8',                             │
│   });                                              │
│                                                    │
└────────────────────────────────────────────────────┘
```

- **Container:** `surface` bg (subtly darker than page bg in light), 1px `border`, 8px radius, 16px padding.
- **Header strip:** 32px tall, `meta` type — language on the left, tiny action icons on the right (`copy`, `expand`). `copy` icon cross-fades to a check glyph for 1.2s when clicked.
- **Code:** `mono` (13.5 JetBrains Mono), syntax-highlighted via **shiki** (one theme per app theme — light uses a warm Paper-like palette; dark uses a warm equivalent).
- **Line numbers:** shown if code is > 8 lines; `text-faint`, right-aligned, 16px gutter, non-selectable.
- **Long code:** max-height 480px, then scrolls internally. Soft top and bottom fade shadows indicate overflow.
- **Horizontal overflow:** scrolls with a scroll shadow, no wrapping by default. A hover action `wrap` toggles soft wrapping.
- **Languages:** first-class support for `typescript`, `javascript`, `cpp`, `c`, `python`, `rust`, `go`, `shell`, `json`, `yaml`, `sql`, `html`, `css`, `markdown`, `diff`, `toml`. Unknown languages fall back to no highlighting.

---

## 3 · Quote block

References an earlier message. Used when the operator drags a previous block into the composer, or when an agent cites a specific prior block in its response.

```
    │  coder · 2 turns ago
    │  I split retry from fallback in runtime-backend.cpp.
    │  The retry policy now lives in its own helper…
    │
    │  ─ in reply
    ↓
    Let's also extract the fallback-model selection into a
    separate strategy object.
```

- **Left border:** 2px, color = author's agent tint (or neutral `border-strong` for user quotes).
- **Indent:** 24px left padding inside the border.
- **Header row:** `meta` type — `<author name> · <relative time>` with a small 20px avatar flush-left.
- **Quoted body:** `body-compact` (14 / 1.55), truncated to 3 lines by default, with an inline `… show more` link. Preserves the original block type (quoting a code block renders as a mini code block inside the quote).
- **Reply body:** appears **below** the quote, full `body` type, no container. A small `─ in reply` meta line separates them.
- **Clicking the quote header** scrolls to and briefly highlights (1s soft accent wash) the source message in the conversation.

---

## 4 · Thinking block

The agent's explicit thinking trace when it produced one. Shown collapsed by default.

Collapsed:

```
    ◌  thought for 4.2s · 680 tokens                ▸
```

Expanded:

```
    ◌  thought for 4.2s · 680 tokens                ▾
    │
    │  The retry loop needs to distinguish between transient
    │  failures and terminal ones. Looking at the current
    │  implementation, everything goes through the same path…
    │
```

- **Collapsed form:** a single `meta`-type row. Leading glyph `◌` (Lucide `brain` at 14px, `text-faint`). `ui`-weight text, `text-muted` color. A small `▸` chevron on the right.
- **Expanded form:** same header with `▾` chevron. Below: a left-border (1px `border`) inset at 16px, italic prose in `body-compact`, `text-muted`. Generous leading (1.7).
- **Never** shown as primary content. It's supporting.
- **Animation:** 140ms height + fade on toggle.
- **Multiple thinking blocks in one turn:** each is independent; the turn trailing-meta row aggregates `thought 3x · 2.1s total`.

---

## 5 · Tool block

A tool was invoked. The block variant is chosen by tool category; they share a common frame.

### Common frame

- **Container:** `surface` bg, 1px `border`, 8px radius, 12px padding on all sides.
- **Header strip:** 28px tall, aligned between `border-strong` top of content
  - Left: category icon (14px Lucide, category-tinted) + tool name (`meta`, lowercase) + one-line input summary (`body-compact`, `text-muted`)
  - Right: duration (`meta`, `text-faint`), status glyph
- **Body:** the variant-specific content (below). Collapsed to a compact preview by default; click the header to expand fully into the detail pane (`]` on focused, or click anywhere on the header).
- **Category tints** (used only on the icon, 2px left tint bar, and nowhere else):
  - file — slate #6B7580
  - shell — iron #5C6370
  - mcp — violet-warm #7B6D8A
  - memory — amber-deep #9E7B3A
  - orchestration — sage #6B8475
  - automation — rose-warm #9E6B6B
  - skill — teal-warm #5B7C7C
  - search — neutral #7A756B
  - script / background — stone #737373

### Variants

#### 5a · Shell

```
    ⌘  shell · ran `cargo build`                      0.14s  ✓
    ─
    Compiling orangutan-runtime v0.1.0
    Finished dev [unoptimized + debuginfo] target(s) in 0.14s
```

- Output rendered as `mono` in a subtle `surface-raised` block inset below the header.
- ANSI color codes respected (use `ansi-to-html` or similar).
- Max 8 lines preview; "show more in detail pane" link if truncated.
- Error states get the `err` left-border treatment (2px).

#### 5b · File edit

```
    ≡  file · edited runtime-backend.cpp            +12 −3  ✓
    ─
    ─── runtime-backend.cpp ─────────────────────────
      42   fn retry_policy() -> Policy {
    + 43       if error.is_auth() { return Policy::Abort; }
    + 44       if error.is_transient() { return Policy::Retry; }
    + 45       Policy::Fallback
    - 43       Policy::Retry
      46   }
```

- Header shows path + additions/deletions.
- Diff rendered with language-aware syntax highlighting, warm red (`err` hue) for removals, warm green (`ok` hue) for additions.
- Preview: the first hunk, max 8 lines. Full diff opens in detail pane.

#### 5c · Search (fd / rg / grep)

```
    🔍  search · rg "repeat_effect_until"             0.06s  ✓
    ─
    runtime-backend.cpp:84
      84  .let_value([](auto& state) { return repeat_effect_until(…) })
    
    runtime-backend.hpp:12
      12  using ::stdexec::repeat_effect_until;
    
    3 more results · open in detail pane
```

- Results grouped by file with a file-path header.
- Match line shown with context (1 line above, 1 below) at line-numbered `mono-small`.
- Match itself has a subtle copper highlight.
- Truncated to 3 file groups in preview.

#### 5d · MCP

```
    ◈  mcp · github.create_issue                     1.3s  ✓
    ─
    Created issue #427 in orangutan/orangutan
    https://github.com/orangutan/orangutan/issues/427
```

- Structured return rendered as a compact prose summary if the tool provides one, otherwise as a collapsed JSON block.
- URLs in the return auto-unfurl as link-preview blocks below the tool block.

#### 5e · Memory

```
    ◉  memory · remembered                            ✓
    ─
    type · user
    key · preferred-editor-mode
    content · operator prefers search_replace edit_mode for hot paths
```

- Shows structured fields.
- Hover over the key shows the full content in a non-modal popover.

#### 5f · Orchestration

Tool calls that spawn or message other agents are **promoted to delegation blocks** (block type 6), not rendered as tool blocks.

### Status glyphs

```
  ↻  running — spins softly (only shared pulse animation)
  ✓  succeeded
  ✗  failed (accompanied by 2px err left-border on the whole block)
  ⏸  awaiting approval (see approval block)
```

---

## 6 · Delegation block

When the agent delegates to a worker or teammate, render the sub-conversation **inline** — not as a card, not as a button, but as a nested, quoted turn.

```
    coder

    I'll check the retry semantics with research first.

    ┌──  (left border 2px, research's accent tint)
    │   [research avatar 32]  research                   2s
    │
    │   The current loop re-enters on any error kind,
    │   including auth. That's the root cause here —
    │   auth failures should be terminal.
    │
    │   ─ details · 1 step
    └──

    Based on that, I've made auth failures short-circuit
    before the retry…
```

- **Left border:** 2px in delegatee's accent tint (seeded per agent).
- **Indent:** 24px left inset from the border.
- **Header:** 32px avatar + name (delegatee's tint) + relative time.
- **Body:** can contain any block type recursively, including another delegation. **Nested depth in main column: max 2.** Deeper trees show a collapsed `… 2 more levels · open trace` row that opens the full tree in the detail pane.
- **Type:** delegated content uses `body-compact` (14 / 1.55) to reinforce the "quoted" feel.
- **Live updates:** if the delegation is still running, the nested block shows its own working stream (see block 15) and status row in the header.
- **Worker vs teammate distinction:** subtle — workers have a single run; teammates show a small mailbox chip in their header when they have queued messages from other agents.

Clicking anywhere on the delegation header scrolls to / focuses the delegatee's full session if it exists as a separate session, or opens the detail pane with the full trace.

---

## 7 · Approval block

Permission prompts render inline, ephemeral. They replace whatever working-stream state was running above them.

```
    coder is waiting for your approval
    
    ⌘  shell · git push origin main
    
    requested · just now      · signature 3f2a91…
    
    Approve      Approve for session      Deny
```

- **Container:** no card; just 24px vertical padding bracketed by 1px `border` top and bottom (hairline separators). A subtle `surface` tint (5% lighter/darker than bg) fills the full column width.
- **Primary label:** `coder is waiting for your approval` in `ui` type (14 weight 450), `text`.
- **Tool row:** same visual as the tool block's header, inline, `text`.
- **Meta row:** `meta` type — requested time + signature digest (click to expand full signature in detail pane).
- **Buttons:** **text buttons**, not filled pills. 16px between them.
  - `Approve` — copper color, bold-ish (weight 520), keyboard hint `A` on hover
  - `Approve for session` — `text`, medium weight, keyboard hint `S`
  - `Deny` — `text-muted`, keyboard hint `D` (denials are intentionally deemphasized; you should see `Approve` first)
- **Keyboard:** `A`, `S`, `D` act on the focused approval block; `tab` cycles between the three buttons.
- **Resolution:** on click or keyboard action, the block collapses (180ms) into a single line: `─ approved: shell git push origin main · via A`. That line remains in the turn as a record; click opens the full signature in the detail pane.
- **Expired / denied:** denial collapses to `─ denied: shell git push origin main · via D`. Color `text-muted`.

---

## 8 · Memory citation block

Two parts: **inline footnote markers** within the response text, and a single **footer row** at the end of the turn.

### Inline footnote markers

Superscript numerals in the prose that mark which clauses were grounded by which memory.

- **Numeral style:** Source Serif 4 Italic, 0.7em, copper-tinted at 70% opacity, 2px left margin from preceding word.
- **Hover:** 200ms delay → non-modal popover (320px max, 12px padding, `surface-raised`, `border` 1px, 8px radius, small tail pointing up). Shows the memory's type tag + key + first 2 lines of content + "open".
- **Click:** opens the memory in the detail pane, pinned to the `Grounded by` section.

### Footer row

After the response body, before the trailing-meta row:

```
    ⟡  grounded by 3 memories: preferred-editor-mode, retry-semantics, +1
```

- `meta` type, `text-muted`.
- Leading glyph `⟡` (Lucide `asterisk` 12px, or a custom 4-pointed star).
- Names are truncated; `+N` counts omitted ones. Click the whole line → detail pane.

This is the **only** place serif numerals appear in the entire app. Their presence is deliberate — a typographic signal that these are *references*.

---

## 9 · Image block

Images attached by operator or returned by an agent tool.

- **Container:** 12px radius, 1px `border`, no padding.
- **Image:** max-width 100% of column (680px effective after padding), preserving aspect ratio. Max-height 560px; taller images render at 560 tall and activate lightbox on click.
- **Caption:** optional; `meta` type, `text-muted`, 8px below image.
- **Lightbox:** click → full-viewport dark overlay (bg rgba(0,0,0,0.85)), image centered with 40px margin. `esc` or click-outside closes. Arrow keys navigate between images in the same turn.
- **Loading state:** warm-toned skeleton (see `06-widgets.md`) filling the aspect-ratio box.
- **Alt text:** if provided, shown as a very faint caption in the bottom-left of the image, fading in on hover.

---

## 10 · Attachment block

File shared — non-image.

```
   ┌──────────────────────────────────────────────┐
   │  [icon]  config.json                          │
   │          2.4 KB · application/json     ⎘  ⤓  │
   └──────────────────────────────────────────────┘
```

- **Container:** `surface`, 8px radius, 1px `border`, 12px padding. Full column width.
- **Icon:** 32px Lucide `file` variant by type (`file-text`, `file-code`, `file-archive`, `file-image` — image actually goes to block 9).
- **Name:** `ui` type, `text`, ellipsis at 1 line.
- **Meta:** `meta` type, `text-muted` — size + MIME type.
- **Actions:** `copy path` (⎘) and `download` (⤓) icons flush right, 14px.
- **Click:** opens in the detail pane (for text/code/json/yaml/csv/pdf/md) with the appropriate viewer. Binary types: download only.

---

## 11 · Link preview block

URL unfurled into a rich card.

```
   ┌─────────────────────────────────────────────────────┐
   │  [favicon] reference.anthropic.com                  │
   │                                                     │
   │  Claude models overview                             │
   │  A guide to the Claude model family, their          │
   │  capabilities, and how to choose between them…      │
   │                                                     │
   │                                        [thumbnail]  │
   └─────────────────────────────────────────────────────┘
```

- **Container:** `surface`, 12px radius, 1px `border`, 16px padding.
- **Header:** 16px favicon + domain in `meta` type, `text-muted`.
- **Title:** `h-section` (15 / 1.35 weight 520) — OpenGraph `og:title` or page `<title>`.
- **Description:** `body-compact`, `text-muted`, max 2 lines.
- **Thumbnail (optional):** right-floated, 120×72 rounded-8px image.
- **Whole card is clickable** → opens in a new tab. Hover: subtle bg wash, 100ms.
- **Loading state:** skeleton with domain + 3 lines of gray bars.
- **Fallback:** if unfurling fails, render as a plain link in prose style (no card).

---

## 12 · Table block

For structured tabular data (agent output, memory search results, etc.).

```
   | type     | key                   | importance | age  |
   | -------- | --------------------- | ---------- | ---- |
   | user     | preferred-editor-mode | 0.8        | 2d   |
   | feedback | retry-semantics       | 0.6        | 5d   |
```

- **Container:** full-column width, 1px `border` (hairlines only between rows, no verticals), 8px radius on the outer frame.
- **Header row:** 36px tall, `meta` type, lowercase, `text-muted`, tracked +0.02em, `surface` bg.
- **Data rows:** 40px tall, `body-compact`, `text`.
- **Cell padding:** 12px horizontal, 0 vertical (centered by line-height).
- **Hover:** subtle `surface` wash on the row, 80ms.
- **Numbers:** tabular-nums, right-aligned in numeric columns.
- **Long values:** ellipsis at 1 line; full value in a hover tooltip.
- **Max rows in-line:** 10. Over that, show `10 of 42 rows · open in detail pane`. Detail pane shows all rows with sort + filter.

---

## 13 · Diff block

Standalone diff presentation (may also appear as a file-edit tool block variant).

```
    ─── retry-policy.hpp ─────────────────────  ⌘⎘  ⤢
      1   #pragma once
      2
    + 3   #include <expected>
    + 4
      5   namespace orangutan::execution {
    + 6       enum class RetryKind { Transient, Auth, Terminal };
      7   }
```

- Renders like a code block but with `+` / `−` gutter and warm red/green highlighting (never saturated).
- Header shows file path + copy/expand actions.
- Supports unified and split view; split requires a viewport ≥ 960px and opens in detail pane.

---

## 14 · Callout block

For info / warning / error annotations within agent responses.

```
   ┌──  (2px left border in status tint)
   │  ⚠  The auth failure mode was undocumented — I've added
   │     a comment in runtime-backend.cpp:58 explaining the
   │     short-circuit behavior.
   └──
```

- **Left border:** 2px, color by kind: `ok` · `warn` · `err` · `text-muted` (info).
- **Icon:** 16px Lucide flush-left (`info`, `alert-triangle`, `alert-octagon`, `sparkles`).
- **Body:** `body-compact`, `text`.
- **No background fill.** The border carries the semantic weight.

---

## 15 · Working stream block

Ephemeral. Shown during a running agent turn **above** where the final response will go. See `03-chat.md` for full behavior.

Summary:
- Each phrase is a one-line human-readable status (`reading runtime-backend.cpp`, `editing retry-policy.hpp`, `running cargo build`).
- `ui` type, `text-muted`. Most recent at bottom.
- Limited to last 3 visible; older roll out with 120ms fade + collapse.
- Trailing copper caret `▍` blinks at 1.1s until first token streams.
- On first streamed token: collapses upward (160ms) into the trailing-meta row.

---

## Block interaction patterns (shared)

### Hover actions

Every block shows a tiny actions strip at its top-right after 150ms hover:

```
  ⎘ (copy)   ❝ (quote)   ⤢ (expand in detail pane)   ⋯ (more)
```

Icons are 14px Lucide, `text-muted`, fade in over 100ms. Strip fades out on leave.

### Focus ring

When keyboard focus lands on a block (`j`/`k` navigation), a 2px copper outline with 4px offset appears around the block. This is the only visible focus style; it appears only with `:focus-visible`.

### Right-click menu

Right-click anywhere in a block → contextual menu (soft popover, not OS native):

- Copy
- Copy markdown
- Quote
- Pin to detail pane
- Export block as… (markdown / json / image if tool)
- Report issue (only for agent blocks)

### Sharing and export

Any block can be exported individually or as part of a turn. The export generates clean markdown (or PDF for longer runs) with the block's metadata preserved as frontmatter.

---

## A note on density

With all these blocks available, it is tempting to stack them. Resist. A well-designed response uses **at most 3 non-text blocks** per turn. The agent should narrate the structural moves in prose (text blocks) and use structural blocks (tools, diffs, tables) only when the information genuinely requires them.

This is an authorial constraint on the agent's output format as much as a UI rule. Expose it in the agent's system prompt: "Prefer prose and inline code. Use tool, diff, code, and table blocks only when the content cannot be expressed clearly in prose."
