# 02 · Shell

The app shell — what is visible on every page. Three zones plus a summoned detail pane and a command palette.

```
┌──────┬────────────────┬───────────────────────────────────────────────┐
│      │                │                                               │
│ Tab  │  Sub-sidebar   │        Main content                           │
│ rail │                │                                               │
│ 56px │  260px         │        (flexible, with internal max widths)   │
│      │  collapsible   │                                               │
│      │                │                                               │
│      │                │                                               │
└──────┴────────────────┴───────────────────────────────────────────────┘
                                                  ┌──────────────────┐
                                                  │  Detail pane     │
                                                  │  440px           │
                                                  │  summoned only   │
                                                  └──────────────────┘
                                                  (slides over main,
                                                   non-blocking, 8% dim)
```

**Command palette (`⌘K`)** overlays everything, centered, 640px wide. Used for navigation, search, and actions.

---

## Tab rail (56px, vertical, fixed left)

Persistent thin rail, always visible. Top to bottom: a small logo mark, page tabs, a spacer, user avatar. Background: `surface`. Border-right: 1px `border`.

### Contents

```
┌────────┐
│   ⌘    │   ← app mark, 32px, monogram
├────────┤
│   💬   │   Chat            (1)
│   ⚯    │   Agents          (2)
│   ◯    │   Memory          (3)
│   ⏱    │   Automations     (4)
│   ✦    │   Skills          (5)
│   ⌬    │   Channels        (6)
├────────┤
│   ⚙    │   Settings        (,)
│   ☀/☾  │   Theme toggle
│  [av]  │   Operator avatar (32px)
└────────┘
```

*(Icons shown here as placeholders; implement with Lucide: `message-square`, `boxes`, `sparkles`, `clock`, `wand-2`, `radio-tower`, `settings`, `sun`/`moon`.)*

### Tab cell

- 40 × 40 px hit area, centered icon (20px Lucide, 1.5 stroke)
- Default: `text-muted`
- Hover: `text` color + subtle bg `rgba(0,0,0,0.04)` (light) / `rgba(255,255,255,0.04)` (dark). 100ms transition.
- Active: `text` color + a 3px vertical accent bar flush-left (copper). No background change for the cell itself. The bar is the only persistent use of the accent in the shell.
- Tooltip on hover: appears after 400ms delay, 100ms fade in. Format: `Chat (⌘1)` — page name + keyboard shortcut. Right-aligned flush to the rail.

### Keyboard

- `⌘1`–`⌘6` jump to each page
- `⌘,` opens Settings
- `⌘\\` toggles the sub-sidebar visibility
- `⌘K` opens the command palette
- `⌘/` opens keyboard shortcut cheatsheet
- `g` then letter: `g a` Agents, `g m` Memory, `g c` Channels, `g s` Skills, `g t` Automations (*t*ime-based)

### Notifications (badges)

On any tab where something actionable is pending, a small 6px status dot appears in the bottom-right of the icon. Only three statuses ever produce a dot:

- `warn` — pending approval somewhere
- `ok` — new teammate mailbox message
- `err` — failed run or automation

Never numbered. Never animated beyond the single shared pulse rule.

---

## Sub-sidebar (260px)

Context-dependent. Each page owns its content. Always has:

- A **header** row (48px tall): page title in `h-section` type + a single primary action button flush-right (e.g., "New chat", "New agent", "New automation"). No icons in header, no secondary actions — those live in the context menu or palette.
- A **search / filter** row (40px tall) when the page benefits from one. Search input is borderless, with a `text-faint` placeholder and a 14px Lucide search icon. Focus state: 1px border `border-strong`.
- A **scrollable body** with the page-specific list.
- An **empty state** when there is nothing to show: centered illustration + one muted line. See `06-widgets.md`.

Collapsible with `⌘\\`; when collapsed, the main content reflows flush to the tab rail. Smooth 160ms transition. A thin strip (2px) of `border` remains visible so the operator knows it exists.

### Per-page content

| Page | Sub-sidebar content |
|------|---------------------|
| Chat | Session list grouped by recency; each row has avatar + title + 6px agent tint dot + hover timestamp |
| Agents | All agent definitions (builtin + config + directory); grouped by source; each row has avatar + name + small role chip |
| Memory | Filter chips (type, age) + memory list; each row has type tag + key + 1-line content preview |
| Automations | Upcoming / Active / Paused groups; each row has name + next-fire countdown + small category icon |
| Skills | Installed skills; each row has name + activation count across agents |
| Channels | Channels grouped by kind (Web / CLI / QQ); each row has channel name + bound agent avatar + JID count |
| Settings | Section navigation only (Workspace / Appearance / Permissions log / Shortcuts / API keys / About) |

Rows in the sub-sidebar are **uniform 44px tall** across pages; density does not vary. Active row: subtle bg wash, 1px left accent bar at 2px width, no icon change.

---

## Main content area

Takes the remaining viewport width. Each page owns its layout. Common patterns:

- **Chat:** a single centered column (max 720px) with 96px top padding.
- **Agents / Memory / Skills:** a grid of cards (3 columns at 1280px+, 2 columns below) with 32px gutters. Page title `h-page` at the top-left, 64px from the top.
- **Automations:** a timeline strip at the top (horizontal scroll of next 24h) + a list below.
- **Channels:** a two-panel layout: channel list on the left (internal), active channel detail on the right.
- **Settings:** a centered column (max 680px), a single `h-section` per subsection, generous spacing.

Horizontal padding inside the main area: **48px** at ≥1280px viewport, **32px** below. Vertical padding to page top: **64px** by default, **96px** for chat.

---

## Detail pane (right slide-over)

A single surface that replaces every modal in the app.

- **Width:** 440px. Drag handle on the left edge snaps to 380 / 440 / 520.
- **Background:** `surface`, 1px `border` on the left edge, single 1-of-opacity ground shadow.
- **Backdrop:** main content behind is dimmed 8%; the pane does NOT block interaction with the main area (you can keep scrolling the chat while tools are open in the pane).
- **Enter:** 160ms ease-out, translate from 100% to 0%.
- **Dismiss:** `escape`, click outside the pane, or press the small close `×` at top-right (Lucide `x`, 16px).

### Inside

**No tabs at the top.** A single vertical scroll of sections, each with a small uppercase tracked `h-section` heading in `text-faint`. Example: a tool's detail view might have sections `INPUT`, `OUTPUT`, `METADATA`. A memory's detail view might have `CONTENT`, `GROUNDING HISTORY`, `ACTIONS`.

**Header of the pane** (48px tall): a small contextual title (e.g., `shell · ran cargo build`) in `ui` type, + breadcrumbs if the pane was reached by drilling (delegation inside a tool inside a memory). Close `×` flush-right.

Only **one** detail pane is ever open. Opening a new detail replaces the current contents with a fast cross-fade (80ms).

---

## Command palette (`⌘K`)

Universal navigation, search, and actions. Always accessible.

- **Container:** 640px wide, max-height 480px, centered vertically at ~35% viewport from top. `surface-raised` background, 16px radius, 1px `border`, one ground shadow.
- **Enter animation:** 140ms, scale 0.98 → 1.00 + fade.
- **Input:** single 48px tall search field at top; placeholder `Search or type a command…`; autofocus; left-aligned 16px Lucide `search` icon in `text-faint`.
- **Scope prefix:** typing `>` offers scope chips. Example: `>agents`, `>sessions`, `>memory`, `>channels`, `>automations`, `>skills`, `>commands`. Scope is selected with `Tab`; visible as a chip left of the caret in the input.
- **Result rows:** 40px tall; avatar (24px) or Lucide icon on the left, title, right-aligned muted meta (e.g., "updated 2m ago", "next fire in 4m"), optional shortcut hint on far right.
- **Sections:** results grouped under small tracked uppercase labels; no section when only one kind of result exists.
- **Keyboard:** arrow keys + enter. `esc` closes. `⌘⏎` opens in detail pane (when applicable). `⌘⇧⏎` opens in a new session (when applicable).
- **Empty state:** when no input, show recent items and 3–5 "recommended actions" (e.g., "Start a new chat with primary", "Approve 1 pending request").

### Scopes

Every scope has at minimum: list, full-text search, and a canonical action on enter.

| Scope | On enter | Extras |
|-------|----------|--------|
| `>agents` | open a new chat with that agent | ⌘⏎ opens agent detail page |
| `>sessions` | open that session | metadata: agent, last turn time |
| `>channels` | drill into channel's JIDs | shows bound agent |
| `>automations` | open most recent run | ⌘⏎ opens automation detail |
| `>memory` | open memory in detail pane | type/age filters via `type:user`, `age:>30d` |
| `>skills` | toggle skill for current session | ⌘⏎ opens skill manifest |
| `>commands` | run the command | includes all slash commands |

When no scope is typed, results are mixed and ranked by a simple relevance score.

---

## Global keyboard reference

```
Navigation
  ⌘K          palette
  ⌘/          shortcut cheatsheet
  ⌘1–⌘6       jump to page
  ⌘,          settings
  ⌘\\          toggle sub-sidebar
  g <letter>  jump by first letter: a=agents m=memory c=channels s=skills t=automations

In conversation
  ⌘⏎          send message
  ⇧⏎          newline in composer
  esc         close detail pane / palette / escape from editing
  ]           toggle detail pane on last focused item
  e           expand focused block
  c           collapse focused block
  r           reply / quote the focused message

Global
  ⌘N          new chat (in Chat page) / new X (contextual on other pages)
  ⌘F          search within current page
  ⌘D          duplicate the focused item (agent, automation, memory)
```

The cheatsheet (`⌘/`) is a centered modal-sized card — the ONE exception to the "no modals" rule — that lists shortcuts grouped by section. Dismissed with `esc` or `⌘/` again.
