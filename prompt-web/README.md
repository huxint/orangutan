# Orangutan Web — Frontend Design Prompt

Complete specification for the Orangutan web frontend. Hand the whole folder (or any subset) to a new Claude Code session to carry the design forward.

**The existing HTTP routes will be redesigned.** Design the data contract alongside the UI; do not shape the UI to existing endpoints.

## Design summary (one paragraph)

A quietly modern, multi-page web app for operating **Orangutan**, a C++23 multi-agent runtime. Reference points: **claude.ai, ChatGPT, Linear, Arc, Notion**. Warm neutral palette with a single copper accent. Chat is the hero surface; other pages (Agents, Memory, Automations, Skills, Channels, Settings) are reachable from a thin left tab rail and a 260px context sub-sidebar. The conversation is composed of rich content **blocks** — text, thinking, tools, delegations, memory citations, approvals, images, code, diffs, tables, link previews — each block well-designed enough to earn its visual weight. Avatars are deterministic two-tone gradients. Motion is minimal, typography does the work, nothing theatrical, nothing that looks like an IDE.

## How to use this prompt

Load the files in order 01 → 07 for a cold start. Each file is self-contained enough that you can also load just the section you are working on. `README.md` is the index; it stays out of the context window unless needed.

## File index

| File | Contents |
|------|----------|
| `01-foundation.md` | Project context · design thesis · visual language (palette, typography, iconography, spacing, motion) · avatar generation |
| `02-shell.md` | Three-zone layout · tab rail · sub-sidebar · detail pane · command palette · keyboard shortcuts |
| `03-chat.md` | Chat page: session list · conversation surface · composer · empty state · streaming behavior |
| `04-blocks.md` | Every content block type: text · quote · thinking · tools (variants) · delegation · approval · memory citation · image · code · attachment · link · table · callout · working stream |
| `05-pages.md` | Agents · Memory · Automations · Skills · Channels · Settings pages |
| `06-widgets.md` | Avatars · status indicators · chips · badges · tooltips · skeletons · mentions · scroll shadows · empty-state art · kbd hints |
| `07-build.md` | Tech stack · state shape · SSE event contract · build order · v1 scope |

## Non-negotiables

- No IDE gutters, no pipe-character rails, no monospace in chat prose.
- No modals. Ever. The right-side detail pane replaces them.
- One accent color (copper). Everything else is warm neutral + agent tints.
- Two typefaces total: Inter Variable + JetBrains Mono.
- Respect `prefers-reduced-motion` — all animation must degrade gracefully.
- Block designs are the craft surface. Get one block right before doing the next.

## Start here

If you are a new session reading this for the first time, begin by reading `01-foundation.md` and `03-chat.md`, then implement the build order in `07-build.md`. Ask the operator about palette values, type scale, and column width **before** adding features — calibrate the feel on an empty screen first.
