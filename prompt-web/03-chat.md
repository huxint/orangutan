# 03 · Chat Page

The primary surface. Session list on the left (in the sub-sidebar), conversation in the center, composer pinned to the bottom.

---

## Sub-sidebar: Session list

Standard sub-sidebar (see `02-shell.md`).

- **Header:** page title `Chat` + primary button `New chat` (opens a new session picker — see below).
- **Search:** full-text over session titles and content.
- **Body:** sessions grouped by recency — `Today / Yesterday / Previous 7 days / Previous 30 days / Older`. Group labels are `meta` type in `text-faint`.
- **Row:** 44px tall
  - Left: 32px avatar of the bound agent
  - Center top: session title (ui type, ellipsis at one line)
  - Center bottom: muted 1-line preview (meta type, `text-muted`)
  - Right on hover: relative timestamp (`2m`, `1h`, `yesterday`, `Mar 14`)
- **Active row:** subtle bg wash, 2px left accent bar, title color `text`.
- **Context menu (right-click or `···` on hover):** Rename · Duplicate · Fork from here · Export · Archive.

### Channel-originated sessions and automation runs

**Not in this list.** They live under `>channels` and `>automations` scopes in the palette. Keeping this list to operator-initiated chats is what makes it scannable.

### New chat picker

When the operator clicks `New chat` or presses `⌘N`:

1. A popover (not a modal; use a lightweight floating surface) appears anchored to the button.
2. It shows the agent list: primary at top, teammates below, separated by a 1px divider.
3. Each row: 32px avatar + name + 1-line `meta` description.
4. Arrow keys + enter selects; starts a new session with that agent and focuses the composer.
5. Typing filters the list.

---

## Conversation surface

Main area. **Single centered column, max-width 720px, 48px horizontal padding outside.** Top padding 96px from the main area's top; bottom padding 160px (composer clearance).

Font: `body` (16 / 1.60) throughout, with block variants using `body-compact` (14 / 1.55).

### Turn structure

A **turn** is one user message + one agent response. Turns are separated by 48px of vertical space. Within a turn, the message header → body → trailing meta gap is 12px. There are no dividers between turns — spacing alone separates them.

```
   [user avatar 40] You                             2:14 pm
                    (user message body)


                    
   [agent avatar 40] coder                          just now
                    (agent working block, then streaming prose,
                     with any rich blocks interspersed)

                    ─  details · 4 steps · 2 tools · 2.1s
```

- **Header row:** 40px avatar + name label + timestamp. The name label for an agent is in that agent's seeded accent tint (low-sat). For the user it is simply `You` in `text`.
- **Avatars** follow the algorithm in `01-foundation.md`. The operator's avatar can be uploaded; agents' are generated.
- **Content column** starts at the avatar's right edge + 12px (so content aligns at roughly 52px from the left edge of the conversation column).
- **Trailing meta row** appears after an agent response, in `meta` type: `─  details · 4 steps · 2 tools · 2.1s`. Clicking `details` opens the detail pane with the full trace.

### Empty state

When a session has no turns yet:

- Conversation column blank except for a centered vertical composition:
  - Agent's avatar at **96px**, centered horizontally
  - Below it: agent name in `display` type (36 / 520)
  - Below that: one `meta` line — `primary · claude-sonnet-4 · 42 memories · idle`
  - 96px margin between avatar and viewport center
- Composer at the bottom as always. Nothing else.

This is the signature first impression — get it perfect before anything else.

### Streaming and the working block

Before the agent's final prose exists, the agent response block shows a **working area**. This replaces the IDE-like "step rail" from earlier drafts.

```
   [coder avatar]  coder                            working · 0:03

                   reading runtime-backend.cpp
                   editing retry-policy.hpp
                   running cargo build

                   ▍
```

- Each line is a short, human-readable phrase (authored by the runtime, not mechanically generated), `ui` type, `text-muted`.
- Only the last **three** phrases are visible. Older ones roll out with a 120ms height collapse + fade.
- A blinking copper caret `▍` sits at the tail from the moment the agent starts, and remains until streaming begins.
- The `working · 0:03` timestamp counts up in the header, replacing the normal timestamp. On completion it is replaced by `2.1s`.

When the first token of the final response streams in, the working area **collapses upward** in a 160ms animation into the trailing meta row's `details` link. The prose begins streaming beneath.

### Block-based content

Inside a message body — user or agent — content is composed of **blocks**. Each block type has a specific visual treatment. See `04-blocks.md` for the full catalog.

A single message can mix any number of blocks. Blocks stack vertically with 16px between them. Paragraphs inside a text block use 16px spacing (one blank line). Block-level indentation (e.g., delegation quotes) does not nest deeper than two levels in the main column — deeper trees are opened in the detail pane.

### Selection, copying, and quoting

- Select any text inside a block to copy with `⌘C`.
- Hover a block to reveal a tiny action strip at its top-right: `copy`, `quote`, `⋯`. Strip fades in on 100ms hover, fades out on leave.
- `quote` on any block drops a **quote block** into the composer referencing the source turn — see `04-blocks.md#quote`.
- Right-click any block for the context menu: Copy · Quote · Copy markdown · Pin to detail pane · Export.

### Virtualization

Below 300 turns, scroll naturally. Above 300, virtualize with `react-virtuoso`. The virtualization must preserve scroll anchors when new content arrives (stick to bottom if already at bottom; otherwise stay put with a small "new messages ↓" pill in the bottom-right).

---

## Composer

Pinned 40px above the viewport bottom, centered on the 720px column.

### Layout

```
┌──────────────────────────────────────────────────────┐
│                                                      │
│  message coder …                                     │
│                                                      │
│  [chips row, only when present]                      │
│                                                      │
│                                                   ↗  │
└──────────────────────────────────────────────────────┘
```

- **Container:** `surface`, 12px radius, 1px `border`, ground shadow. Min-height 56px, grows to 240px, then scrolls internally. 20px horizontal padding, 14px vertical.
- **Placeholder:** `message <agent name> …`, lowercase, italic, `text-muted`. The agent name specifically grounds the operator in *who* they are writing to.
- **Chips row (conditional):** appears above the input when there are attachments, skill toggles, or `@` mentions. 32px tall row of chips (see `06-widgets.md`). Hidden when empty.
- **Send button:** bottom-right, 28×28 circular button with a 16px Lucide `arrow-up` icon. Copper when there is input, `text-faint` when empty. During a running turn, it becomes a **stop** button (Lucide `square`, `err` color).
- **Context window indicator:** 1px hairline along the top edge of the composer container, filling left-to-right in copper as the turn's context utilization climbs. Invisible under 60%. Nudges to `warn` over 90%.

### Input

- `⏎` sends. `⇧⏎` newline. `⌘⏎` sends even when the input is still focused on autocomplete.
- **No visible icon toolbar.** Every action is a slash command or shortcut.
- **Paste:** automatic detection of pasted content → the right block type:
  - Image → image block chip in the chips row
  - File → attachment chip
  - URL → link-preview chip (with a toggle to send as plain text)
  - Long text (>500 chars) → offer to send as a code block or attachment
  - Structured JSON → code block with `json` language
  - Diff / patch → code block with `diff` language

### Slash commands

Typing `/` opens a quiet autocomplete popover above the composer. Rows: 40px, icon + command + short description + keyboard hint.

```
/agent <name>        switch current session's bound agent
/skill <name>        toggle a skill for this turn / this session
/delegate <agent>    tell current agent to delegate; pre-fills prompt
/memory <query>      open memory detail pane scoped to query
/fork                fork the session from the last turn
/model <name>        override model for this turn
/stop                stop the running turn (same as ⌘.)
/clear               clear conversation history in this session
/summarize           ask the agent to summarize the conversation so far
/export              export the session as markdown
```

### Mentions

- `@` opens an agent / session mention picker. Selecting drops a chip into the chips row and is consumed as context when the turn runs.
- `#` opens a session mention for cross-referencing (handy when asking an agent to "look at what #refactoring-retries concluded").

### Attachments

Click-to-attach has no button — drag a file onto the composer, or paste. File types supported: images (png, jpg, webp, svg), text (txt, md, log), code (all languages via shiki), data (json, yaml, csv), pdf (rendered preview in detail pane). Each attachment shows as a chip in the chips row with a thumb (for images) or icon (for others). Remove with `×` on the chip or backspace when chip is focused.

---

## When a turn is running

- The composer remains usable — typing queues the next turn. Send with `⏎` appends it to the queue; the next message is shown as a dim preview block at the bottom of the conversation.
- `⌘.` or the stop button cancels the running turn.
- If an approval prompt appears mid-turn (see `04-blocks.md#approval`), the run pauses; the composer's placeholder switches to `respond to coder's request above …` as a subtle nudge, but the composer stays usable if you want to send a side-note.

---

## Keyboard navigation in the conversation

- `j` / `k` or `↓` / `↑`: move the focus ring block by block
- `enter`: expand a focused collapsible block
- `e` / `c`: expand / collapse focused block
- `r`: quote the focused block into the composer
- `]`: open the focused block's detail in the right pane
- `⌘C`: copy the focused block's content
- `/`: focus composer and start a slash command
- `esc`: clear focus / blur composer

Focus ring is a 2px copper outline with 4px offset, visible only when navigating by keyboard (`:focus-visible`).
