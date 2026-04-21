# 05 · Pages (beyond Chat)

Six secondary pages, each reached from the tab rail. Every page shares the shell from `02-shell.md`: tab rail + sub-sidebar + main area. Every page is **designed to reduce, not expand, the amount of UI work the operator has to do** — primary actions up front, less-frequent ones behind palette or context menu.

Common layout elements:

- **Page title** at the top-left of main area, `h-page` type (22 / 1.30 weight 520). 64px from the main area's top.
- **Page subtitle / meta** below title, `meta` type, `text-muted`. Usually a count or status.
- **Primary action button** at the top-right of the main area, flush with the title baseline. Copper outline (1px copper border, transparent fill, copper text), 32px tall, 12px padding, 14px icon + label.
- **Grid** for item lists: 3 columns at ≥1280px, 2 columns at 900–1280px, 1 column below. Gutter 24px. Cards have `surface` bg, 1px `border`, 12px radius, 20px padding.

---

## 2 · Agents page

Manage agent definitions: view, create, edit, duplicate, delete, test.

### Sub-sidebar

- Header: `Agents · 6 defined` + primary button `New agent`
- Search: `name, description, tools`
- Body: agents grouped by `source`
  - **Builtin** (cannot be edited)
  - **Config** (from config.json)
  - **Directory** (from .orangutan/agents/)
- Each row: 32px avatar + name + small `meta` role tag (`leader` · `teammate` · `worker`)

### Main: list view

Grid of agent cards (default layout, toggle with `⌘⇧L` for list).

```
┌──────────────────────────────────────────────┐
│  [avatar 40]  coder                   chat → │
│               teammate · in primary's team   │
│                                              │
│  Surgeons C++23 code with clinical restraint │
│  and an eye for performance.                 │
│                                              │
│  model · claude-sonnet-4                     │
│  tools · 12 enabled  ·  skills · 4           │
│  memories · 218  ·  used 3h ago              │
│                                              │
│  edit  ·  duplicate  ·  ⋯                    │
└──────────────────────────────────────────────┘
```

- **Card hover:** bg wash 80ms; `chat →` link reveals from `text-faint` → copper.
- **Card actions** (bottom row, `meta` type): text links, 12px apart. `⋯` opens a popover menu (delete, export, view file path).
- **Header row:** avatar + name + `chat →` link on right (copper, always visible).
- **Role chip:** `meta` lowercase, positioned after name.
- **Description:** `body-compact`, `text-muted`, max 2 lines.
- **Stats:** `meta` type, arranged as a key · value pair per line.

### Main: agent detail

Clicking a card opens a **full-page detail view** (not the detail pane). The sub-sidebar stays; the main area becomes:

- **Hero header:** 96px avatar, name in `h-page`, description, `chat →` primary button
- **Sections:** separated by 64px
  - **Identity** — avatar, name, description, source file path, leader/teammate role
  - **Model** — provider, model, fallbacks, thinking budget, temperature (if configurable)
  - **Tools** — grid of enabled tools with small category icons; click to disable; "add tool" button
  - **Permissions** — allow / deny / ask tables, one per scope (see permissions evaluator in runtime)
  - **Skills** — pinned skills for this agent; toggle enable per skill
  - **Prompt addendum** — text viewer / editor (only editable for non-builtin agents); mono font, syntax highlighted
  - **Activity timeline** — last 20 runs as a compact list, each row shows status, duration, tokens, trigger source
  - **Memory summary** — count by type, last 5 created, link to memory page filtered by this agent

### New agent flow

Primary button `New agent` opens a **right-slide detail pane** (yes, the same pane) with a single-column form:

1. Name (required)
2. Description (textarea)
3. Role (radio: leader, teammate, worker)
4. Model (select)
5. Tools (multi-select)
6. Prompt addendum (optional, mono textarea)

No wizards, no steps. One form. On save, a new agent file is written to `.orangutan/agents/<name>.toml` and the list updates optimistically.

---

## 3 · Memory page

Memory is a core entity. Make it feel like a well-curated library, not a database admin UI.

### Sub-sidebar

- Header: `Memory · 1,284 records` + primary button `New memory`
- Filter chips (see `06-widgets.md`): `type: all/user/feedback/project/reference`, `age: all / <7d / <30d / older`, `importance: all / high / medium / low`, `agent: all / <agent picker>`
- Search: `content, key, category`
- Body: flat memory list, sorted by `last_accessed DESC`. Each row:
  - 8px type-color dot
  - key in `ui`
  - 1-line content preview in `meta`, `text-muted`
  - right-aligned: age (`3d`, `2w`, `4mo`)

### Main: grid of memory cards

```
┌──────────────────────────────────────────┐
│  ● user                        edit  ⋯  │
│                                          │
│  preferred-editor-mode                   │
│                                          │
│  Operator prefers search_replace         │
│  edit_mode for hot paths, and hashline   │
│  for cold/initialization code.           │
│                                          │
│  importance ████░  ·  age 4d             │
│  recalled 12 times · last 3h ago         │
└──────────────────────────────────────────┘
```

- **Type dot:** 8px, semantic color:
  - `user` — copper #BB6F37
  - `feedback` — sage #6B8475
  - `project` — indigo-warm #7C6D90
  - `reference` — stone #7A756B
- **Key:** `h-section`, mono-ish sans (actually Inter; keys use kebab-case which looks natural in sans).
- **Content:** `body-compact`, `text`, max 3 lines, "… more" link if truncated.
- **Importance bar:** 5-segment block bar at `meta` size, copper filled, stone empty.
- **Meta row:** age + recall count.
- **Card click:** opens memory in detail pane (not full page; memory operations are quick).

### Memory detail pane

```
CONTENT
  (full content, editable in-place)
  
TYPE · user              AGENT · primary

IMPORTANCE  ████░ 0.8    CREATED  2025-04-17 14:32

GROUNDING HISTORY
  · turn 428 · 3h ago · used in "refactor retry loop"
  · turn 381 · 2d ago · used in "setup edit mode"
  · turn 356 · 4d ago · used in "initial config"

ACTIONS
  [edit]  [forget]  [promote]  [graft with…]  [export]
```

- Editing is in-place; `⌘S` or click-outside saves; `esc` cancels.
- `forget` is destructive; requires a 2-second hold on the button (progress ring fills) — the only confirmation pattern in the app. Prevents accidents without adding a modal.
- `graft with…` opens a quiet inline combobox to merge with another memory.
- `promote` bumps importance by 0.1.

### Memory mirror

If `memory.mirror_enabled` is on in config, a small badge in the page header shows `mirrored to MEMORY.md`. Click opens the file path in the platform's file explorer (if local) or copies the path.

---

## 4 · Automations page

Scheduled runs. Most visual-layout work lives here.

### Sub-sidebar

- Header: `Automations · 7 active` + primary button `New automation`
- Body: grouped
  - **Firing soon** (next 24h)
  - **Active**
  - **Paused**
- Each row: 8px category dot + name + countdown (`in 4m`, `in 2h`, `tomorrow 9:00`)

### Main: timeline + list

**Top:** a horizontal timeline of the **next 24 hours**, 120px tall, full-width minus page padding.

```
  ─────╂─────╂─────╂─────╂─────╂─────╂─────╂─────╂─────
   now   3h   6h   9h   12h   15h   18h   21h   24h
        ●           ●●         ●              ●     ●
     heartbeat    batch...    digest       standup  retry
```

- Horizontal rule with hour ticks every 3h.
- Each automation firing in the window is a 6px dot at its firing time.
- Label appears above the dot on hover; stays on permanently for dots under the cursor or focused.
- Clicking a dot opens that automation's detail view in the main area.
- Clusters (>2 dots within 30min) collapse to a single dot with a count (e.g., `●3`).

**Below:** a list view of all automations, sortable.

```
| name              | schedule       | agent    | next fire  | last run         | category  |
| heartbeat         | every 5m       | primary  | in 2m      | 3m ago · ok      | heartbeat |
| nightly-digest    | 0 22 * * *     | research | tomorrow   | yesterday · ok   | report    |
| weekly-review     | 0 9 * * MON    | primary  | Mon 09:00  | 6d ago · ok      | report    |
```

- Table styled like a Table block (block type 12).
- Hover row: subtle bg wash + trailing `⋯` menu (pause/resume, run now, edit, delete).

### Automation detail view

Full page (replaces main area). Sections:

- **Header:** name, category pill, status, next fire countdown, buttons (`Run now`, `Pause`, `Edit`, `Delete`)
- **Schedule** — visual cron editor (see below)
- **Agent & prompt** — which agent runs, and the prompt template
- **Delivery** — silent vs notify, targets list
- **Run history** — table of past runs: timestamp, duration, status, tokens, summary excerpt; click opens the run as a read-only chat session
- **Heartbeat rules** (if category = heartbeat) — ack max chars, HEARTBEAT_OK sentinel config

**Visual cron editor:** three rows of pickers instead of raw cron text. Row 1: "Run every [ 5 ] [ minutes / hours / days / weeks ]". Row 2 (conditional): "At [ time picker ]". Row 3 (conditional): "On [ day-of-week chips ]". A collapsible "advanced" disclosure exposes the raw cron expression for power users.

---

## 5 · Skills page

Skills are capability bundles. Make their affordances discoverable.

### Sub-sidebar

- Header: `Skills · 18 installed` + primary button `Install skill`
- Filter chips: `active/inactive`, `per-agent`
- Search

### Main: grid of skill cards

```
┌──────────────────────────────────────────┐
│  ✦  git-deep                             │
│     enabled for · coder, primary         │
│                                          │
│  Advanced git operations: rebase,        │
│  cherry-pick, bisect, reflog analysis.   │
│                                          │
│  tools · 6       prompt fragment · 1.2k  │
│  activation · manual                     │
│                                          │
│  enable for coder  ·  manifest  ·  ⋯    │
└──────────────────────────────────────────┘
```

- Standard card.
- `enable for <agent>` toggles per-agent state; ghosted when already enabled.
- `manifest` opens the skill's manifest JSON + prompt fragment in the detail pane.

### Skill detail pane

Sections: manifest · prompt fragment · tools provided · activation rules · per-agent matrix. The matrix is a small grid: rows = agents, columns = [enabled, auto-activate]. Checkboxes.

---

## 6 · Channels page

External ingress points. This is where multi-channel nature becomes visible.

### Sub-sidebar

- Header: `Channels · 4`
- Body: 3 groups
  - **Built-in**: `web` (you), `cli`
  - **QQ bots**: each bot (primary-bot, coder-bot, research-bot)
  - **Other** (future: Slack, Discord…)

Row: 32px avatar (the bot's, or a channel-kind icon) + name + bound-agent name + small status dot

### Main: channel detail (selected channel)

Two-panel layout:

**Left panel (320px):** JID list for this channel.
- Header: `Conversations · 42`
- Search + filters (`active/dormant/blocked`)
- Row: 24px avatar (JID-seeded) + JID name + last message time + unread count

**Right panel:** detail of the channel itself (when no JID is focused) or a live view of the JID's conversation (when one is).

When no JID is focused, the right panel shows:
- Channel name + kind (web / cli / qq-bot) + bound agent
- Credential scope preview (masked)
- Per-channel permission overrides
- Throughput stats: messages last 24h / 7d / 30d
- Recent activity feed

When a JID is focused, the right panel becomes a **chat interface identical to the Chat page** — same composer, same blocks, same avatars. The operator can read or even speak into the remote conversation (with a clear `on behalf of <channel>` note above the composer in `meta` type).

**This is one of the most important unifications in the design.** The same conversation UI serves your own chats and every remote channel's conversations. It is what makes the chat paradigm not break under Orangutan's multi-channel nature.

---

## 7 · Settings page

Configuration and operational concerns. Centered, reading-column layout.

### Sub-sidebar

- Section navigation only:
  - Workspace
  - Appearance
  - Permissions log
  - Keyboard shortcuts
  - API keys
  - About

### Main: one section per scroll view

Centered column (max 680px). Each section is separated by 64px vertical space with an `h-section` heading.

#### Workspace

- Workspace root (path, editable, file picker)
- Default agent
- Session auto-save toggle
- Memory mirror toggle + path
- Journal directory

#### Appearance

- Theme (light / dark / system)
- Font size scale (0.875x / 1.0x / 1.125x / 1.25x)
- Density (comfortable / compact)
- Reduced motion (auto follows system, override available)
- Monospace font (preset list)

#### Permissions log

- A table (block type 12) of every permission decision made: timestamp, tool, signature, decision (approved / denied), decider (`operator` / `rule`), agent, session
- Filters: decision, agent, date range
- Export as CSV

#### Keyboard shortcuts

- Categorized list from `02-shell.md`
- Each row: action description + `kbd` chips for the combo
- Non-customizable in v1; spec adds `rebind` action in later versions

#### API keys

- List of configured profiles with masked keys (`sk-ant-api03-…abc`)
- Password-protected reveal (not in v1, but show masked by default)
- Never log or display decrypted secrets anywhere in the UI

#### About

- Version, build hash, build time, Git commit
- Build options (qq_channel on/off, etc.)
- Link to the docs and the repo
- A small footer: `Orangutan · made in C++23`

---

## Design notes across all pages

- **Empty states:** each page has a distinct empty state illustration (line art only, single stroke, `text-muted`) + a single supportive sentence + the primary action. See `06-widgets.md`.
- **Skeletons:** all lists and grids use warm-toned skeletons during load. No spinners.
- **Optimistic updates:** all create / edit / delete operations are optimistic with rollback on failure. Failures surface as toasts (see `06-widgets.md`).
- **Responsive:** these pages assume viewport ≥ 1024px for v1. Below that, graceful degradation (single-column, sub-sidebar becomes a drawer) is a v2 concern.
- **Deep-linking:** every page and detail view is URL-addressable. Agent detail: `/agents/<key>`. Memory detail: `/memory/<id>`. Channel: `/channels/<name>`. Channel + JID: `/channels/<name>/<jid>`.
