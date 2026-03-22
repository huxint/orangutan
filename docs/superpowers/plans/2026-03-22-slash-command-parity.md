# Slash Command Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unify meaningful slash-command behavior across CLI, channel, and web chat while preserving intentional surface-specific differences.

**Architecture:** Extract the shared slash-command semantics into reusable helpers, route channel and web through those helpers, and leave terminal-only capabilities such as `/multi` and `/quit` in CLI. Keep web-specific constraints such as read-only channel sessions unchanged.

**Tech Stack:** C++23, GoogleTest, React 19, TypeScript

---

### Task 1: Define Shared Slash-Command Scope

**Files:**
- Modify: `src/app/cli-ui.cpp`
- Modify: `src/app/repl.cpp`
- Modify: `src/app/channel-serve.cpp`
- Modify: `src/features/web/web-routes.cpp`

- [ ] **Step 1: Identify commands that should be shared**
- [ ] **Step 2: Keep CLI-only commands (`/quit`, `/exit`, `/multi`, `/clear`, `/history`, `/save`, `/skills`, `/tools`) out of web/channel parity**
- [ ] **Step 3: Keep intentionally different web behavior such as read-only channel sessions**

### Task 2: Implement Shared Slash-Command Handling

**Files:**
- Create: `src/app/slash-commands.hpp`
- Create: `src/app/slash-commands.cpp`
- Modify: `src/app/channel-serve.cpp`
- Modify: `src/features/web/web-routes.cpp`
- Modify: `src/app/repl.cpp`

- [ ] **Step 1: Write failing parity tests for shared commands in web and channel**
- [ ] **Step 2: Add a shared parser/dispatcher for session-oriented slash commands**
- [ ] **Step 3: Route channel through the shared dispatcher**
- [ ] **Step 4: Route web through the shared dispatcher before agent execution**
- [ ] **Step 5: Keep CLI behavior stable while reusing shared helpers where practical**

### Task 3: Align Help Text And Verify

**Files:**
- Modify: `src/app/cli-ui.cpp`
- Modify: `web/src/components/chat/ChatInput.tsx`
- Modify: `tests/app/channel-serve-test.cpp`
- Modify: `tests/features/web-chat-test.cpp`

- [ ] **Step 1: Make help text describe only commands actually supported on each surface**
- [ ] **Step 2: Add a lightweight web hint for supported slash commands**
- [ ] **Step 3: Run targeted tests for channel and web slash-command parity**
