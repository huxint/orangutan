# 06 · Widgets & Small Components

The library of reusable small things. Every widget here must be implemented once and reused everywhere — consistency is how a multi-page app feels cohesive.

---

## Avatar

See `01-foundation.md` for the generation algorithm. This section covers UI behavior.

### Component API

```ts
<Avatar
  seed: string          // entity id: agent key, user id, JID
  name?: string         // optional — provides letter overlay
  size?: 24|32|40|48|64|96
  imageUrl?: string     // overrides generated gradient
  status?: 'idle'|'running'|'waiting'|'error'|'mailbox'
  className?: string
/>
```

### Status ring

When `status` is provided, render a 2px ring around the avatar with 2px offset, colored by status:

- `idle` — no ring
- `running` — `ok` color + shared pulse animation
- `waiting` — `warn` color (no pulse)
- `error` — `err` color (no pulse)
- `mailbox` — small 6px dot at top-right corner in `text` color

The ring is **outside** the avatar, so avatar dimensions stay constant.

---

## Status dot

6px circle, rendered as `<span>` with flex-shrink-0. Four kinds:

| Color | Token | Semantic |
|-------|-------|----------|
| #7FA978 | `ok` | succeeded, running-ok, active |
| #D4A03B | `warn` | waiting, paused, pending approval |
| #C8614C | `err` | failed, denied, terminated |
| #8F8A7F | `muted` | idle, dormant, inactive |

Optional pulse for `ok` when status is `running`. Otherwise static.

---

## Chip

A small, inline, rounded label. Comes in multiple variants.

### Variants

**Plain chip** — text only
```
  ·  meta type  ·  text-muted  ·  no background
```

**Outlined chip** — for filters, toggles, categories
```
  ┌──────────────┐
  │  user        │   12px · weight 450 · 4px radius · 1px border
  └──────────────┘
  
  padding: 3px 8px · line-height: 1
```

Colors:
- Default: `border` 1px + `text` color
- Active: `border-strong` 1px + `text` + subtle `surface` bg
- Selected: copper 1px + copper text + 8%-copper bg

**Filled chip** — avoid. Only for status that already has a dot form.

### Dismissible chip (for composer attachments, mentions)

Add a 12px `×` icon at the right inside the chip, `text-muted` → `err` on hover. 4px gap from label. `backspace` when focused removes.

---

## Badge

Distinct from chip; badges are numeric indicators, not labels.

- 16px min-width, 16px height, pill shape (full radius)
- `meta` type (12 / 1.0)
- Centered numeric content
- Color combos: `text-muted` on `surface-raised` (default), copper-tinted for unread

Rules:
- No badge over 99 — display `99+`.
- Badges appear in sub-sidebar rows (unread counts), palette (session counts), and nowhere else. Never in the tab rail (tab rail uses a status dot instead).

---

## Tooltip

Non-modal hover popover for small contextual info.

- Background: `surface-raised`
- Border: 1px `border`
- Radius: 6px
- Padding: 8px 10px
- Type: `meta` (12 / 1.40)
- Max width: 280px
- Shadow: single 1-of-opacity ground shadow
- Appears 400ms after hover start, disappears 100ms after leave
- Positioned with automatic flipping (avoid viewport edges)
- Small 6px tail pointing to the anchor
- Never animated beyond a 100ms fade

Use for keyboard shortcut hints, truncated text previews, and clarifying icon meanings. Never for critical information — if it needs a tooltip to be understood, it's too opaque.

---

## Popover

Larger than a tooltip, for rich content.

- Background: `surface-raised`
- Border: 1px `border`
- Radius: 8px
- Padding: 12px (default) or 16px (dense)
- Shadow: single 1-of-opacity ground
- No tail unless anchored to a small trigger
- Enter: 140ms fade + 98% → 100% scale
- Dismiss: `esc`, click outside, click the trigger again
- Never contains form inputs — use the detail pane for those

Used for: memory preview on footnote hover, mention details, automation next-run tooltip (when enriched).

---

## Button

Three variants.

### Primary
```
  ┌──────────────────────┐
  │     Send             │   copper bg · text #FBF8F0 · 14px weight 500
  └──────────────────────┘
  
  padding: 8px 16px · radius: 8px · 1px copper border
```

Used exclusively for: send button in composer, primary approval button, primary page action buttons.

### Secondary
```
  ┌──────────────────────┐
  │     Cancel           │   transparent bg · text color · 14px weight 450
  └──────────────────────┘
  
  padding: 8px 16px · radius: 8px · 1px border color
```

### Ghost / text button
```
         Approve           copper text, underline on hover
         Cancel            text-muted, underline on hover
```

No container at all. Used for tertiary actions, inline confirmations, menu items.

### Icon button

```
  ┌────┐
  │  ⎘ │   32x32 hit area · transparent bg · icon 16px · text-muted
  └────┘
```

Hover: `text` color, subtle `surface` bg wash, 100ms.

---

## Kbd hint

Keyboard shortcut display.

```
  ⌘  K       single: small kbd cap
  ⌘⇧⏎       compound: joined with thin separator
```

- `mono-small` type (12.5px)
- `surface-raised` bg
- 1px `border`
- 3px 6px padding
- 4px radius
- Always displayed with 2px letter-spacing between keys when joined visually

On buttons, kbd hints appear to the right of the label with 8px gap, `text-faint` color. On menu items, flush-right.

---

## Skeleton

Loading placeholder. Never a spinner.

- Base color: `surface` (slightly darker than bg)
- Shimmer: 1.8s linear-gradient moving left-to-right, from 0% to 100% lightness +4%
- Shape: rectangle with appropriate aspect ratio, 4px radius (6px for cards)
- Multiple skeletons in a list shimmer in phase (one animation, not staggered)

Patterns:

- **Message skeleton:** avatar (40×40) + name bar (80×12) + 3 text bars (varied widths: 90%, 75%, 55%).
- **Card skeleton:** title bar (60% width) + meta bar (30%) + content bars (3x varied).
- **List row skeleton:** avatar (24×24) + name bar (40%) + timestamp bar (15%).

With `prefers-reduced-motion`: static at the mid-lightness state, no shimmer.

---

## Toast

Transient notification.

- Position: bottom-right, 24px from edges
- Stack vertically with 8px gap, newest at bottom
- Max 3 visible; older ones dismiss
- Width: 360px
- Background: `surface-raised`, 1px `border`, ground shadow
- Padding: 12px 16px
- Type: `ui`
- Enter: 200ms slide-in from right + fade
- Dismiss: 4s auto (6s for `warn`, never auto for `err`), or click the `×`

Kinds:

- `info` — neutral; small 6px `muted` dot on left
- `ok` — 6px `ok` dot
- `warn` — 6px `warn` dot
- `err` — 6px `err` dot + explicit dismiss button required

Never use toast for information the operator must act on — those need inline UI (approval block) or a status change somewhere they'll look.

---

## Scroll shadows

When a scrollable area has content beyond the viewport, show subtle fade shadows at the ends.

- Top shadow: 24px gradient from `bg` opacity 0 → 0.4
- Bottom shadow: inverse
- Visible only when there is content in that direction
- Implemented as CSS mask or pseudo-elements

Applied to: chat conversation, sub-sidebars, long lists, code blocks, the detail pane.

---

## Section divider

Horizontal rule with an optional label.

```
  ────────  UPCOMING FIRINGS  ──────────────────────
```

- 1px `border` hairline across the full section width
- Label: 11px uppercase, tracked `+0.06em`, `text-faint`, in a `bg`-colored box clipping the line
- Used in settings sections, grouped lists, and page subdivisions

---

## Breadcrumbs

For nested contexts (detail pane drilling, channel-JID navigation, agent detail sub-sections).

```
  channels / coder-bot / user-0421
```

- `meta` type, `text-muted` separators, `text` for the current page
- Slash separator with 8px padding
- Each segment is clickable (navigates back)
- Max length: 4 segments; deeper paths show `…` in the middle

---

## Mention chip

Inline reference to another agent or session.

```
  @coder           avatar (24) + name · copper text · 1px copper-tint border
  #refactor-retry  chip with # prefix · muted border
```

- Acts like a link: clicking navigates to the referenced entity
- Used in composer inputs, message bodies (if agent includes one), and memory content

---

## Progress hairline

1px tall progress indicator. Used for:

- Context window utilization (top edge of composer)
- Long tool operations (top edge of tool block)
- Batch operations (top edge of detail pane)

Fill animates with a 200ms ease-out transition. Color follows status: copper for normal progress, `warn` for >90%, `err` for failed.

---

## Hold-to-confirm button

The only confirmation pattern in the app. Replaces destructive modals.

- Button looks like a normal secondary button at rest
- On mousedown (or spacebar when focused), a copper progress ring fills from 0 to full over **2 seconds**
- When full, the destructive action fires and the button shows a 400ms check glyph
- Release before full: the ring drains over 400ms and no action fires

Used for: `forget` memory, `delete` agent, `delete` automation, `clear history`, `force-stop` run.

With `prefers-reduced-motion`: falls back to a two-step confirm (click once → button re-labels to `really delete?` for 3 seconds → click again to confirm).

---

## Empty state illustration

Each page and empty list has its own illustration. All are **monochrome line art**, `text-faint` color, 1.5 stroke, 160×120 max size, centered above the supportive text.

Library (one illustration each):

- **Chat (no sessions):** a single envelope with faint motion lines
- **Agents (no agents):** three overlapping small circles
- **Memory (no memories):** a small leaf / sprout
- **Automations (no automations):** a pocket watch face with no hands
- **Skills (no skills):** a constellation of 4 stars
- **Channels (no channels yet):** a radio antenna
- **Search (no results):** a magnifier with a dashed outline
- **Error:** a small broken line segment

All illustrations are simple enough to be rendered as inline SVG (under 1KB each). No decorative noise.

---

## Timestamp

Two modes:

- **Relative** (default): `just now`, `2m ago`, `1h ago`, `yesterday`, `Mar 14`
- **Absolute** (on hover or long-press): `2025-04-21 14:32:18 GMT+8`

Auto-updates every 30 seconds while visible.

- Type: `meta`, `text-muted` (or `text-faint` for less emphasis)
- Format via a shared util — every timestamp in the app uses the same formatter

---

## Filter chip row

Used in sub-sidebars and page filters.

```
  [ all ]  [ user ]  [ feedback ]  [ project ]  [ reference ]
```

- Outlined chips, 24px tall, 4px radius
- One chip is always "active" (copper border + copper text + 8%-copper bg)
- Click toggles
- Multi-select rows use `[ ✓ active ]` with a checkmark glyph
- Keyboard: tab moves between chips, space/enter toggles

---

## Search input

Single consistent design across the app.

```
  ─────────────────────────────────────
   [search icon]  search agents…
  ─────────────────────────────────────
```

- Borderless by default, 1px border-bottom on focus
- Leading 16px Lucide `search` icon in `text-faint`
- `ui` type, placeholder in `text-faint`
- `⌘F` focuses the nearest search input (page-scoped)
- `esc` clears; if already empty, blurs

---

## Sortable table header

For tables that support sorting (permissions log, automations list, memory list).

- Click header → sort by column
- Active column: small 12px chevron (`▲` asc, `▼` desc) in copper, flush-right of label
- Inactive columns: no chevron, label `text-muted`
- Shift-click: multi-sort (v2 feature)

---

## Context menu

Right-click or `⋯`-click menu. Not native OS menus.

- Background: `surface-raised`
- Border: 1px `border`
- Radius: 8px
- Padding: 4px (tight around items)
- Shadow: single ground shadow
- Min-width: 180px
- Items: 32px tall, 12px padding, `ui` type, hover fills row with `surface` wash
- Kbd hints right-aligned
- Dividers: 1px `border` with 4px vertical margin, label optional
- Destructive items: `err` text color

---

## What's NOT a widget

Don't build these — use the existing primitive instead:

- **Dropdown / select** → use the command palette scoped to the field's purpose, or an inline combobox
- **Accordion** → use section headings with explicit expand/collapse state per section
- **Tabs** → use the right-slide detail pane or page navigation
- **Progress spinner** → use skeleton or the 1px progress hairline
- **Confirmation modal** → use hold-to-confirm
- **Snackbar with actions** → use inline UI in the place the action should happen

If you catch yourself reaching for one of these, stop and reconsider.
