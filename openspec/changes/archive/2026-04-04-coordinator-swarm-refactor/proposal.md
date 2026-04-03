## Why

The current subagent system is a simple single-level parent-child delegation model: a parent agent spawns child agents that execute tasks in separate threads, but children cannot spawn their own children, there is no inter-agent communication, and the parent must manually poll or block-wait for results. This limits the system to trivial fan-out patterns and prevents sophisticated multi-agent workflows like research-then-implement pipelines, parallel exploration with synthesis, or coordinated team-based problem solving. Adopting a Coordinator Mode + Agent Swarms architecture (inspired by Claude Code's proven implementation) will unlock these capabilities while providing a cleaner, more extensible foundation.

## What Changes

- **BREAKING**: Remove the entire `subagent` subsystem including `SubagentManager`, `SubagentRunStore`, all three LLM-facing tools (`subagent_spawn`, `subagent_status`, `subagent_wait`), the config `"subagents"` field, child runtime config builder, and all related wiring in bootstrap/web/channel layers
- **BREAKING**: Replace config `"agents"` schema -- remove `"subagents"` array, add `"coordinator_mode"` and `"team"` configuration sections
- Introduce **Coordinator Mode**: a mode where the main agent becomes a pure orchestrator with a restricted tool set (only `agent_spawn`, `agent_send_message`, `agent_stop`), a specialized coordinator system prompt, and async worker agents that report results via structured notifications
- Introduce **Agent Swarms (Teams)**: a persistent team system where named agents communicate via a file-based mailbox, can work concurrently as in-process coroutines (thread pool), and coordinate through direct/broadcast messaging
- Introduce **Agent Definitions**: a declarative agent definition format (markdown frontmatter or JSON) loadable from config and `.orangutan/agents/` directories, defining each agent's description, allowed tools, model, prompt, and constraints
- Add `SendMessage` tool for inter-agent communication (direct messages, broadcasts, structured protocols like shutdown requests)
- Add `TeamCreate`/`TeamDelete` tools for dynamic team lifecycle management
- Replace SQLite-based `SubagentRunStore` with a file-based team state system (team config JSON + per-agent inbox JSON files)
- Add task notification system: workers report completion/failure via `<task-notification>` XML injected into the coordinator's conversation
- Update web admin API to expose team structure and agent communication state instead of subagent run records

## Capabilities

### New Capabilities
- `coordinator-mode`: Core coordinator orchestration -- restricted tool set, coordinator system prompt, worker dispatch, task notification aggregation, multi-phase workflow (research/synthesis/implementation/verification)
- `agent-swarm-teams`: Team lifecycle management -- team creation/deletion, member registration, file-based team config persistence, agent identity and color assignment
- `agent-mailbox`: Inter-agent communication -- file-based inbox system, direct/broadcast messaging, structured message protocols (shutdown, permission sync), message read tracking
- `agent-definitions`: Declarative agent type system -- markdown/JSON agent definitions, frontmatter schema (tools, model, prompt, constraints), loading from config and directory, built-in agent registry
- `agent-runner`: In-process agent execution -- thread-pool-based concurrent agent execution, context isolation, prompt loop with mailbox polling, idle/shutdown lifecycle management

### Modified Capabilities
<!-- No existing specs to modify -->

## Impact

- **Core source files removed**: `src/subagent/` directory (manager + all types), `src/storage/subagent-run-store.*`, `src/tools/subagent/` directory
- **Core source files modified**: `src/config/config.hpp` (new schema), `src/bootstrap/bootstrap.cpp` (new wiring), `src/bootstrap/identity.cpp` (team/agent identity), `src/bootstrap/agent-runtime.*` (new context fields), `src/bootstrap/channel-serve.*` (team injection), `src/tools/registry/tool-context.hpp` (new context fields), `src/web/` routes and server (team APIs)
- **New source directories**: `src/coordinator/`, `src/swarm/`, `src/tools/coordinator/`, `src/tools/swarm/`
- **Config format**: Breaking change to `config.json` agent schema
- **Build system**: `xmake/targets.lua` and `xmake/tests.lua` updated for new source directories
- **Frontend**: `web/src/components/admin/AgentsPage.tsx` and `AgentTree.tsx` updated for team visualization
- **Tests**: All `tests/subagent/` and `tests/storage/subagent-run-store-test.cpp` replaced with new test suites
- **SQLite dependency**: `subagent-runs.db` eliminated; replaced by JSON files in `~/.orangutan/teams/`
