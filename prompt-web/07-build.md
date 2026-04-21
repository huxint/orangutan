# 07 · Build

Tech stack, state shape, event contract, and iteration plan.

---

## Stack

- **Framework:** React 18 + TypeScript (strict).
- **Bundler:** Vite with `@vitejs/plugin-react-swc`.
- **Routing:** `react-router` v6+ (declarative nested routes mirror the tab rail).
- **State:** Zustand. Single store with slices; selectors with `useStore(state => …)`. No Redux, no MobX.
- **Styling:** Tailwind for layout and design-token utilities; CSS custom properties carry the palette so light/dark theme swapping is just `<html data-theme>`.
- **Animation:** `framer-motion` for the palette + detail pane; CSS transitions for everything else. **No GSAP.** No Three.js. No WebGL.
- **Markdown:** `markdown-it` with plugins for GFM tables, footnotes, task lists.
- **Syntax highlighting:** `shiki` with two themes (warm-paper light, warm-ink dark).
- **Icons:** `lucide-react`, tree-shakable import per icon.
- **Dates:** `date-fns` for formatting, `intl-messageformat` if i18n ever comes.
- **Keyboard:** `cmdk` (the palette uses this primitive). Plain event handlers elsewhere.
- **Virtualization:** `react-virtuoso` (only enabled above 300 turns).
- **Diff:** `diff-match-patch` for rendering, plus syntax highlighting on top.
- **URL preview unfurl:** server-side — the backend should fetch and cache; the UI displays.

**No design-system dependency** like MUI, Chakra, Radix themes. Build small primitives in-house because every block has a distinct opinion. Radix *primitives* (headless) are OK for popovers, dialogs, dropdown lists — just don't take their theming.

---

## State shape (Zustand)

Organized by slice. Selectors derive view state; no duplicated mirror of server state.

```ts
type Store = {
  // chat state
  sessions: Record<SessionId, Session>
  sessionOrder: SessionId[]
  activeSessionId: SessionId | null
  
  // agent state
  agents: Record<AgentKey, AgentDefinition>
  
  // channel state
  channels: Record<ChannelId, Channel>
  jids: Record<JidId, Jid>
  
  // memory state (lazy-loaded, paginated)
  memoryCache: Record<MemoryId, Memory>
  memoryIndex: {
    byAgent: Record<AgentKey, MemoryId[]>
    byType: Record<MemoryType, MemoryId[]>
    recent: MemoryId[]
  }
  
  // automation state
  automations: Record<AutomationId, Automation>
  automationRuns: Record<RunId, AutomationRun>
  
  // skills
  skills: Record<SkillId, Skill>
  
  // ephemeral / ui
  ui: {
    subSidebarOpen: boolean
    detailPane: { kind: 'memory' | 'tool' | 'delegation' | 'automation' | null; id: string | null }
    paletteOpen: boolean
    theme: 'light' | 'dark' | 'system'
    fontScale: number
    reducedMotion: boolean
    focusedBlockId: BlockId | null
  }
  
  // connection state
  connection: {
    sse: 'connecting' | 'open' | 'closed' | 'error'
    lastEvent: number
  }
  
  // actions — grouped under namespaces, flat store
  actions: StoreActions
}
```

### Turn representation

A single source of truth for a turn during its lifecycle. The UI reads this shape and renders accordingly.

```ts
type Turn = {
  id: TurnId
  sessionId: SessionId
  user: UserMessage              // prompt + blocks + attachments
  agent: {
    agentKey: AgentKey
    startedAt: number
    status: 'running' | 'awaiting-approval' | 'succeeded' | 'failed' | 'terminated'
    workingPhrases: string[]      // last ≤3 shown live
    blocks: Block[]               // streaming; append as events arrive
    iterations: IterationSummary[] // structural metadata
    grounded: MemoryId[]
    trace: TraceRecord | null     // full structured trace, populated post-turn
    duration: number | null
    tokens: { in: number, out: number } | null
    cost: number | null
  }
}

type Block =
  | { kind: 'text'; content: string }       // markdown
  | { kind: 'code'; language: string; content: string }
  | { kind: 'thinking'; content: string; tokens: number; duration: number }
  | { kind: 'tool'; tool: ToolCall }
  | { kind: 'delegation'; target: AgentKey; subTurn: Turn }
  | { kind: 'approval'; request: ApprovalRequest; resolution?: ApprovalResolution }
  | { kind: 'image'; url: string; caption?: string; alt?: string }
  | { kind: 'attachment'; file: FileRef }
  | { kind: 'link-preview'; url: string; meta: UnfurledMeta }
  | { kind: 'table'; schema: TableSchema; rows: TableRow[] }
  | { kind: 'diff'; path: string; hunks: DiffHunk[] }
  | { kind: 'callout'; kind: 'info'|'warn'|'err'|'ok'; content: string }
  | { kind: 'quote'; source: BlockRef; reply: Block[] }

type ToolCall = {
  id: ToolCallId
  category: 'file'|'shell'|'mcp'|'memory'|'orchestration'|'automation'|'skill'|'search'
  name: string
  input: unknown
  output?: unknown
  status: 'running'|'succeeded'|'failed'|'awaiting-approval'
  duration?: number
  error?: string
}
```

Note: memory citations attach to text blocks via inline markers in the markdown (e.g., `[^mem-17]`), with a `grounded` list on the turn. The markdown renderer emits the superscript + popover inline; the footer row uses the `grounded` list directly.

---

## SSE event contract

Single SSE stream per active session (with a server multiplexer if one connection carries multiple sessions). Events are JSON objects with `type` and `data`. Shape these to match the UI's needs; do not shape them to the existing routes.

### Turn lifecycle

```
turn_started          { turn_id, session_id, agent_key, user_message, started_at }
working_phrase        { turn_id, phrase, phrase_id, index }
block_started         { turn_id, block_id, kind, ordinal }
block_delta           { turn_id, block_id, delta }          // for streaming text blocks
block_completed       { turn_id, block_id, final_content }
tool_call_started     { turn_id, block_id, tool_call_id, category, name, input_summary }
tool_call_completed   { turn_id, tool_call_id, status, output_preview, duration }
approval_requested    { turn_id, tool_call_id, tool, signature, requested_at }
approval_resolved     { turn_id, tool_call_id, decision, decider, resolved_at }
delegation_started    { turn_id, block_id, target_agent, prompt }
delegation_finished   { turn_id, block_id, sub_turn_id, status }
memory_grounded       { turn_id, memory_ids }
iteration_completed   { turn_id, iteration_num, summary }
token_delta           { turn_id, tokens_in, tokens_out }
turn_finished         { turn_id, status, duration, cost }
```

### Other event streams

```
session_created       { session_id, agent_key, title, created_at }
session_updated       { session_id, fields }
session_deleted       { session_id }

agent_status          { agent_key, status, mailbox_count }
agent_definition_changed  { agent_key, fields }

memory_created        { memory }
memory_updated        { memory_id, fields }
memory_forgotten      { memory_id }

automation_scheduled  { automation_id, next_fire_at }
automation_fired      { automation_id, run_id, agent_key, started_at }
automation_completed  { automation_id, run_id, status, duration }

channel_message       { channel_id, jid_id, direction, message }
channel_status        { channel_id, status }

permission_decision   { tool_call_id, decision, rule_id?, decider }

connection_ping       { ts }    // every 20s, used for liveness
```

### Reducer model

State transitions are event-driven. A single reducer consumes each event and updates the store. This makes time-travel debugging, chorology (if later added), and replay straightforward.

Event ordering guarantees:
- Events within a single turn are monotonically ordered.
- Turns within a session are monotonically ordered.
- Cross-session events are NOT globally ordered — each session's stream is independent.

Use a monotonic `seq` field per event for debugging and gap detection.

---

## Request contract (REST-lite)

In addition to SSE, a small request surface for imperative actions:

```
POST   /sessions                        { agent_key, title?, initial_prompt? }  → { session_id }
POST   /sessions/:id/messages           { blocks }                              → { turn_id }
POST   /sessions/:id/stop
POST   /sessions/:id/fork               { from_turn_id? }                       → { new_session_id }
DELETE /sessions/:id

POST   /approvals/:tool_call_id         { decision, scope }

GET    /agents                          → AgentDefinition[]
POST   /agents                          { definition }
PATCH  /agents/:key                     { fields }
DELETE /agents/:key

GET    /memory                          ?type&age&agent&q&cursor  → { items, next_cursor }
GET    /memory/:id
PATCH  /memory/:id                      { fields }
DELETE /memory/:id                      → deletes / forgets
POST   /memory/:id/graft                { target_id }

GET    /automations
POST   /automations
PATCH  /automations/:id
DELETE /automations/:id
POST   /automations/:id/fire            → triggers immediately
POST   /automations/:id/pause
GET    /automations/:id/runs            ?cursor

GET    /channels
GET    /channels/:id
GET    /channels/:id/jids               ?status&cursor
GET    /channels/:id/jids/:jid          → returns session id for continuation

GET    /skills
PATCH  /skills/:id                      { agent_enabled }

GET    /permissions/log                 ?agent&decision&cursor

GET    /settings
PATCH  /settings
```

Shape, names, and path structure are suggestions — tune with the backend rewrite.

---

## Component tree (high level)

```
<App>
  <ThemeProvider>
    <KeyboardProvider>
      <ConnectionProvider>       // SSE, reducer, store
        <AppShell>
          <TabRail />
          <SubSidebar>
            {page-specific sidebar content}
          </SubSidebar>
          <MainArea>
            <Routes>
              <ChatPage />
              <AgentsPage />
              <MemoryPage />
              <AutomationsPage />
              <SkillsPage />
              <ChannelsPage />
              <SettingsPage />
            </Routes>
          </MainArea>
          <DetailPane />          // portaled, conditional
          <CommandPalette />      // portaled, conditional
          <ToastHost />           // portaled
        </AppShell>
      </ConnectionProvider>
    </KeyboardProvider>
  </ThemeProvider>
</App>
```

### Block rendering

```
<BlockList blocks={turn.blocks}>
  <BlockRenderer block={block}>
    {block.kind === 'text' && <TextBlock />}
    {block.kind === 'code' && <CodeBlock />}
    {block.kind === 'tool' && <ToolBlockRouter />}  // dispatches by category
    {block.kind === 'delegation' && <DelegationBlock />}  // recurses into BlockList
    {/* ...all block types */}
  </BlockRenderer>
</BlockList>
```

Every block component receives:
- The block data
- `isFocused: boolean`
- `onFocus()`, `onExpandToDetail()`
- An optional `parentContext` (for nested delegations)

---

## Build order

Iterate in vertical slices. Do not build entire pages before landing a complete single flow.

### v1 — the aesthetic proof

1. **Shell skeleton.** Tab rail with all 7 tabs, sub-sidebar, main area with route outlet, empty chat page. Validate palette, typography, tab rail active states, keyboard shortcuts. (One day.)
2. **Empty chat state.** Agent avatar at 96px, name in display type, meta line, composer docked. Pixel-perfect. (Half day.)
3. **One static turn end to end.** Hardcoded mock turn with avatar, header, one text block response, trailing meta row. Validate line-length, spacing, leading, color. (Half day.)
4. **Streaming.** Mock SSE stream; working phrases block → streaming text block → collapse on first token. (One day.)
5. **Tool block — shell variant.** Full visual including status states. (Half day.)
6. **Tool block — file-edit variant with diff.** Shiki syntax highlighting, warm red/green. (One day.)
7. **Code block + Markdown rendering.** Full markdown pipeline with headings, lists, inline code, links, footnotes. (Half day.)
8. **Detail pane.** Opens from tool block expand; renders full I/O. (Half day.)
9. **Delegation block.** Recursive; one level nested. (Half day.)
10. **Approval block.** Inline; three buttons; resolved-state collapse. (Half day.)
11. **Memory citation markers + footer.** Footnote popover. (Half day.)
12. **Avatars.** Generator + 5 size variants + status ring. Use throughout. (Half day.)
13. **Sessions list in sub-sidebar + session switcher in palette.** (One day.)
14. **Command palette with scopes.** Agents, sessions scopes live; others wire later. (One day.)

**v1 is done when:** the operator can have a complete chat with the primary agent, see tool calls, approve a permission, follow one delegation, and switch between sessions — all feeling right.

### v2 — pages

15. Agents page (list + detail pane).
16. Memory page (grid + filters + detail pane + hold-to-confirm forget).
17. Automations page (timeline + list + detail view + visual cron editor).
18. Channels page (with JID list and embedded chat).
19. Skills page.
20. Settings page (all sub-sections).

### v3 — polish

21. Virtualization for long conversations.
22. Export flows (session → markdown, permissions log → CSV).
23. Deep-linking for all detail pane views.
24. Mobile-adaptive layout (single column, drawer for sub-sidebar).
25. Light theme calibration.
26. `prefers-reduced-motion` QA pass.
27. Accessibility audit (ARIA for chat log, keyboard traps, focus order).

### Not in scope (v4+)

- Voice input / TTS
- Multi-operator / realtime collaboration
- In-app editing of agent prompt addendum with preview
- File picker integration for workspace root
- Push notifications (desktop / browser)
- Localization (strings are English-only until then)

---

## Development conventions

- **File layout:**
  ```
  src/
    app/                       shell, routes, providers
    components/
      blocks/                  one file per block type
      widgets/                 all primitives from 06
      shell/                   tab rail, sub-sidebar, detail pane, palette
    pages/
      chat/
      agents/
      memory/
      automations/
      skills/
      channels/
      settings/
    store/                     zustand slices + reducer
    sse/                       event stream, reducer dispatch
    api/                       REST client
    types/                     shared types
    styles/                    tokens.css, globals.css, shiki-theme.ts
    utils/                     date, hash (for avatar seed), etc.
  ```
- **Naming:** PascalCase components, camelCase everything else. File names match component names.
- **Tests:** Vitest + Testing Library. Test blocks in isolation with storybook-style snapshots of different states. SSE reducer gets its own test file with recorded event streams.
- **Type discipline:** strict TS, `noUncheckedIndexedAccess`, no `any`, exhaustive switch checks for block variants.

---

## Handoff checklist

Before declaring any iteration done:

- [ ] Typography renders correctly at 1x and 1.25x font scale.
- [ ] Light and dark themes both tested.
- [ ] `prefers-reduced-motion` degrades gracefully.
- [ ] Keyboard navigation works for every affordance.
- [ ] No modal dialogs were introduced.
- [ ] No spinner loaders (skeletons only).
- [ ] No colored fills except copper accent and status dots.
- [ ] Every new block type has hover actions and a focus ring.
- [ ] Conversation column stays within 720px max-width.
- [ ] Detail pane is the only right-side surface; not turned into tabs.

If any of these is "no", stop and fix before building more.
