# 01 · Foundation

Project context, design thesis, visual language, avatar system. This is the layer every other file is built on.

---

## What Orangutan is

A single-binary **C++23 multi-agent runtime**. The operator has a **primary agent** (leader) that can spawn **workers** (one-shot) and **teammates** (persistent, mailbox-backed). Each turn runs a **ReAct loop** — thinking, tool calls, results — until a final response.

- **Agent roles:** standalone · leader · worker · teammate
- **Run statuses:** queued · running · idle · succeeded · failed · terminated · abandoned
- **Tools:** file · shell · MCP · memory · orchestration · automation · skill · search · script · background
- **Permissions:** allow / deny / ask rules, with mid-turn approval prompts
- **Memory:** typed (`user | feedback | project | reference`), importance-weighted, age-decayed, retrieved per turn
- **Skills:** capability bundles activated on demand by a `SkillLoader`
- **Automations:** cron/periodic/triggered jobs that spin up a fresh agent run; categories include `heartbeat` (silent when reply is `HEARTBEAT_OK`)
- **Channels:** web, CLI, many QQ bots; each QQ bot binds to one agent and serves many remote conversations (JIDs)

The UI must make all of this reachable and legible **without looking like an ops console.** It is a chat app that happens to run a multi-agent system.

---

## Design thesis

**Write it like a letter, not like a log.** The conversation is the hero. Every other surface exists to keep the conversation uncluttered while still giving the operator real control over agents, memory, automations, channels, skills, and permissions.

Five principles:

1. **Chat-app-shaped.** The benchmark is claude.ai / ChatGPT / T3 Chat, polished further. The primary interaction is typing a message and reading a response. Anything that makes the page feel like a terminal, an IDE, a dashboard, or a log viewer is wrong.
2. **Richness through blocks, not chrome.** Structural variety lives in the *content* of the conversation — well-designed blocks for tools, thinking, delegations, citations, code, images — not in persistent UI rails, panels, or toolbars.
3. **One warm accent.** Copper. Used for the send button, active-state indicators, the agent's typing caret, and approval buttons. Never for decoration.
4. **Typography is the design.** Two fonts. Generous leading. Careful sizing. Real paragraph breaks. This single decision gives the whole app its quality level.
5. **Calm by default; respond when summoned.** No ambient motion. No breathing elements. No drift. Motion only when the user caused a state change.

---

## Visual language

### Palette

Dual theme. Light is the default. Both are warm, low-contrast, with a single accent.

```
Light (default)
  bg              #F6F2E9        warm bone
  surface         #FBF8F0        sidebars, composer, detail pane, cards
  surface-raised  #FFFFFF        popovers, palette
  border          #E0DAC8        1px hairlines
  border-strong   #C8C0AB        block boundaries (quote, callout)
  text            #1C1A16
  text-muted      #6F6A5C
  text-faint      #A8A192

Dark
  bg              #18171A
  surface         #1E1D21
  surface-raised  #26252A
  border          #2C2A2F
  border-strong   #3A3740
  text            #EBE7DE
  text-muted      #8F8A7F
  text-faint      #5A5650

Accent · copper    #BB6F37   (identical in both themes)
Status             ok #7FA978 · warn #D4A03B · err #C8614C   (used as 6px dots, never as fill)
Agent tints        deterministically seeded from agent key; always low-saturation;
                   rendered as a 2px left border or a 6px dot, never as background fill
```

**Gradients, glassmorphism, backdrop blur, colored fills, drop shadows beyond 1-of-opacity ground** — none.

### Typography

Exactly two typefaces. Calibrate every size on a visible block before using it elsewhere.

```
Inter Variable      UI, body, meta, everything except raw tool I/O
JetBrains Mono      code blocks, diffs, raw tool I/O, JIDs, run ids

display         36 / 1.15    weight 520      empty-state headings
h-page          22 / 1.30    weight 520      page titles (Agents, Memory, …)
h-section       15 / 1.35    weight 520      card headers, detail sections
body            16 / 1.60    weight 400      message prose, primary reading
body-compact    14 / 1.55    weight 400      nested prose (delegations)
ui              14 / 1.45    weight 450      buttons, composer text, list rows
meta            12 / 1.40    weight 450      labels, timestamps, tags    · +0.02em · lowercase
mono            13.5 / 1.50                  JetBrains Mono
mono-small      12.5 / 1.50                  for inline code in prose
```

**Rules:**
- Meta labels lowercase, tracked `+0.02em`. Matches the project's spdlog lowercase convention.
- Section headings uppercase tracked `+0.06em`, 11px, `text-faint`. Used sparingly inside the detail pane and settings.
- Numbers are tabular (`font-feature-settings: "tnum"`).
- Target measure for body: **≈ 70–80 characters per line** at the conversation column's 720px max width.

### Iconography

**Lucide**, 16px nominal, 1.5 stroke, `text-muted` by default. Never filled. Never colored. Used sparingly — never more than one icon per row in the chat, never decoratively.

Emoji are never used in chrome. They may appear in user or agent messages as normal text.

### Spacing scale

`4 · 8 · 12 · 16 · 24 · 32 · 48 · 64 · 96 · 128`. Use the larger values more than feels natural; this is how chat apps earn their calm.

### Shape

- Border radius: `4` small chips / `8` cards / `12` composer / `16` detail-pane and palette. Nothing larger.
- Borders: 1px hairline. Use color, not thickness.
- Shadows: one sibling, 1-of-opacity, 1px offset. Used only under composer, palette, and detail pane. Nowhere else.

### Motion

Quiet, functional.

```
reveal (pane, palette)      translate+fade · 160ms / 120ms · ease-out
expand/collapse (block)     height+fade · 140ms
status-dot pulse            opacity 0.5↔1 · 1.4s · only when status is running
streaming caret             1.1s blink · copper · only one caret on screen at a time
new turn                    1-frame fade in · 80ms · never a slide
tab switch                  cross-fade · 120ms
hover                       opacity+bg change · 100ms · no scale transforms
```

**Disallowed:** parallax, drift, breathing, micro-bounces, hover scale, decorative easing curves, loading spinners (use skeletons instead).

With `prefers-reduced-motion`: disable caret blink, status-dot pulse, and all fades. Keep height transitions (they are essential affordances, not decoration).

---

## Avatars

Avatars carry identity for agents, the operator, and remote channel participants (QQ JIDs). They are **generated, not chosen**, so they feel present without requiring config work.

### Algorithm

1. Compute a stable 32-bit hash of the entity's id (agent key / user id / JID).
2. Map the hash to a **gradient pair** drawn from a curated 24-pair palette (see below).
3. Map the hash to a **gradient angle** from the set `{0°, 45°, 90°, 135°, 180°, 225°}`.
4. Optionally overlay the first letter of the entity's name:
   - Thin serif (Source Serif 4 Italic, weight 400)
   - Color: `#FFFFFF` at 55% opacity in light theme, `#1C1A16` at 45% opacity in dark
   - Size: 45% of avatar diameter
5. Shape: **rounded square** with 22% corner radius (superellipse-ish, not a pure circle).
6. Add a 1px inset border at `rgba(0,0,0,0.06)` for definition against light bg, `rgba(255,255,255,0.05)` against dark.

### Gradient palette (24 pairs, all warm, low-saturation)

```
 1  #D4A574  #A6713E    sand → sienna
 2  #B4876D  #6E4A36    almond → chestnut
 3  #C9A178  #8B5E3C    wheat → walnut
 4  #A38A60  #5F4721    olive → bark
 5  #E0B98A  #A37A47    buff → caramel
 6  #C1917B  #7A4A3A    terracotta → brick
 7  #9FA573  #5C6340    sage-warm → forest-warm
 8  #B8A279  #7F6A42    dune → umber
 9  #CFA386  #8B5A3F    coral-muted → mahogany
10  #D3B47E  #9C7940    straw → honey
11  #A59380  #655648    stone → cocoa
12  #C89F7D  #85553A    peach-muted → cedar
13  #968A73  #564C3C    ash-warm → tobacco
14  #B4A385  #7A6A4F    linen → bronze
15  #C89177  #7E4F3C    salmon-muted → chestnut-dark
16  #AB9977  #6B5A3D    khaki → espresso
17  #D6B596  #9F7452    cream-warm → ochre
18  #B89080  #73503F    clay → rust
19  #A89B7F  #605947    jute → moss
20  #C7A88D  #886446    hazel → copper
21  #B8A17C  #7C6840    vellum → spice
22  #A89780  #6A5740    champagne → coffee
23  #B59575  #7A4F36    buckskin → russet
24  #9F8567  #5C4526    taupe → walnut-dark
```

Each pair is deliberately close in hue; the gradient is subtle, never garish. None approach saturated primaries.

### Sizes

| Size | Use |
|------|-----|
| 24px | inline mentions, dense lists |
| 32px | session list, agent chips in rail |
| 40px | **default**; chat message header |
| 48px | page-title surfaces |
| 64px | profile cards, agent detail header |
| 96px | empty states, "meet the agent" moments |

### Overrides

- An agent definition may specify `avatar_image` (path to a square image file); that replaces the generated avatar.
- The operator may upload an avatar for their own identity.
- JID avatars are always generated — we don't fetch remote platform avatars (privacy, and it would require API calls on every render).

### Implementation note

Render as an **SVG** with two `<stop>` nodes in a `linearGradient`, sized by a container div. Cache SVG strings keyed by entity id — avatars are rendered thousands of times across the app and must be cheap.
