## 1. Remove Subagent System

- [x] 1.1 Delete `src/subagent/subagent-manager.hpp` and `src/subagent/subagent-manager.cpp`
- [x] 1.2 Delete `src/storage/subagent-run-store.hpp` and `src/storage/subagent-run-store.cpp`
- [x] 1.3 Delete `src/tools/subagent/` directory (subagent-tool.cpp, register.hpp, register.cpp)
- [x] 1.4 Delete `src/bootstrap/config-builder.hpp` and `src/bootstrap/config-builder.cpp` (subagent child runtime config builder)
- [x] 1.5 Delete `tests/subagent/` and `tests/storage/subagent-run-store-test.cpp` and `tests/integration/subagent-integration-test.cpp`
- [x] 1.6 Remove `SubagentManager` references from `src/bootstrap/bootstrap.cpp` -- remove include, store creation, manager construction, and all wiring
- [x] 1.7 Remove `SubagentManager` references from `src/bootstrap/agent-runtime.hpp` and `src/bootstrap/agent-runtime.cpp`
- [x] 1.8 Remove `SubagentManager` references from `src/bootstrap/channel-serve.hpp` and `src/bootstrap/channel-serve.cpp`
- [x] 1.9 Remove `SubagentManager` references from `src/bootstrap/runtime-control.cpp`
- [x] 1.10 Remove `SubagentManager` references from `src/web/web-server.hpp`, `src/web/web-server.cpp`, `src/web/web-routes.hpp`, `src/web/chat-routes.cpp`, `src/web/admin-routes.cpp`
- [x] 1.11 Remove `subagent_manager`, `allowed_child_agents`, `is_child_run` fields from `src/tools/registry/tool-context.hpp`
- [x] 1.12 Remove `AgentConfig::subagents` field from `src/config/config.hpp` and parsing from `src/config/config-sections-core.cpp`
- [x] 1.13 Remove subagent-related prompt guidance from `src/bootstrap/identity.hpp` and `src/bootstrap/identity.cpp`
- [x] 1.14 Remove `register_builtin_subagent_tools` from `src/tools/register.hpp`
- [x] 1.15 Update `xmake/targets.lua` to remove `src/subagent/*.cpp` sources
- [x] 1.16 Update `xmake/tests.lua` to remove `test-subagent` target and subagent test sources
- [x] 1.17 Verify clean build after all removals

## 2. Config Schema Update

- [x] 2.1 Add `coordinator_mode` (bool), `team_agents` (vector<string>), and `max_concurrent_agents` (int) fields to `AgentConfig` in `src/config/config.hpp`
- [x] 2.2 Parse new fields in `src/config/config-sections-core.cpp`
- [x] 2.3 Update `config.example.json` with new agent configuration examples showing coordinator mode and team agents
- [x] 2.4 Update `AgentRuntimeConfig` in `src/bootstrap/channel-serve.hpp` to carry `team_agents`, `coordinator_mode`, `max_concurrent_agents`
- [x] 2.5 Update `AgentRuntimeBuildInput` in `src/bootstrap/agent-runtime.hpp` to carry new fields
- [x] 2.6 Serialize new fields in `Config::save_to` if applicable

## 3. Agent Definition System

- [x] 3.1 Create `src/coordinator/agent-definition.hpp` -- `AgentDefinition` struct (description, tools, disallowed_tools, model, max_turns, prompt_addendum), `BuiltInAgentType` enum
- [x] 3.2 Create `src/coordinator/agent-definition-loader.hpp/.cpp` -- `AgentDefinitionLoader` class that scans `.orangutan/agents/` for markdown files with YAML frontmatter
- [x] 3.3 Create `src/coordinator/agent-definition-registry.hpp/.cpp` -- `AgentDefinitionRegistry` class that merges built-in, config, and directory definitions with precedence rules
- [x] 3.4 Implement built-in agent definitions: `general-purpose`, `explorer`, `planner`
- [x] 3.5 Write unit tests for agent definition loading and registry

## 4. Agent Mailbox System

- [x] 4.1 Create `src/swarm/mailbox.hpp/.cpp` -- `AgentMailbox` class with inbox per agent, backed by SQLite
- [x] 4.2 Define `MailboxMessage` struct: from, to, text, timestamp, read, type (message/shutdown_request/shutdown_response)
- [x] 4.3 Implement `send(to, message)`, `send_broadcast(message)`, `poll(agent_name) -> vector<MailboxMessage>`, `mark_read(message_ids)`
- [x] 4.4 Add mailbox SQLite table creation to storage initialization (new migration or table in existing store)
- [x] 4.5 Write unit tests for mailbox send/receive/broadcast/persistence

## 5. CoordinatorManager Core

- [x] 5.1 Create `src/coordinator/coordinator-manager.hpp` -- `CoordinatorManager` class with thread pool, agent registry, mailbox, and run tracking
- [x] 5.2 Define coordinator types: `AgentSpawnRequest`, `AgentSpawnResult`, `AgentRunRecord`, `AgentRunStatus` enum (queued/running/succeeded/failed/terminated/abandoned)
- [x] 5.3 Implement `spawn()` -- validate request, create run record, submit to thread pool
- [x] 5.4 Implement `send_message()` -- route message to agent mailbox
- [x] 5.5 Implement `stop()` -- trigger stop_token, mark as terminated after grace period
- [x] 5.6 Implement `shutdown()` -- stop all active runs, join thread pool
- [x] 5.7 Implement agent worker execution: create isolated context, run AgentLoop, report result via task notification
- [x] 5.8 Implement task notification generation: format `<task-notification>` XML and inject into coordinator's conversation
- [x] 5.9 Create `src/coordinator/coordinator-manager.cpp` with full implementation
- [x] 5.10 Write unit tests for CoordinatorManager spawn/stop/shutdown lifecycle

## 6. Team Management

- [x] 6.1 Create `src/swarm/team-manager.hpp/.cpp` -- `TeamManager` class for team CRUD operations
- [x] 6.2 Define `TeamRecord` struct: id, name, description, lead_agent_id, created_at, status
- [x] 6.3 Define `TeamMemberRecord` struct: agent_id, name, agent_key, team_id, joined_at, status
- [x] 6.4 Implement team creation, member registration, team deletion with shutdown signaling
- [x] 6.5 Add team/member SQLite tables to storage initialization
- [x] 6.6 Implement team state recovery on restart (mark active members as abandoned)
- [x] 6.7 Write unit tests for team lifecycle

## 7. Coordinator Tools

- [x] 7.1 Create `src/tools/coordinator/agent-spawn-tool.cpp` -- `agent_spawn` tool implementation
- [x] 7.2 Create `src/tools/coordinator/agent-send-message-tool.cpp` -- `agent_send_message` tool implementation
- [x] 7.3 Create `src/tools/coordinator/agent-stop-tool.cpp` -- `agent_stop` tool implementation
- [x] 7.4 Create `src/tools/coordinator/register.hpp/.cpp` -- registration function for coordinator tools
- [x] 7.5 Write unit tests for each coordinator tool

## 8. Team Tools

- [x] 8.1 Create `src/tools/swarm/team-create-tool.cpp` -- `team_create` tool implementation
- [x] 8.2 Create `src/tools/swarm/team-delete-tool.cpp` -- `team_delete` tool implementation
- [x] 8.3 Create `src/tools/swarm/register.hpp/.cpp` -- registration function for team tools
- [x] 8.4 Write unit tests for team tools

## 9. Coordinator System Prompt

- [x] 9.1 Create `src/coordinator/coordinator-prompt.hpp/.cpp` -- `get_coordinator_system_prompt()` function that returns the orchestrator prompt with 4-phase workflow guidance
- [x] 9.2 Create `src/coordinator/coordinator-mode.hpp/.cpp` -- `is_coordinator_mode()` check, `get_coordinator_allowed_tools()` returning restricted tool list
- [x] 9.3 Update `src/bootstrap/identity.cpp` -- replace `append_subagent_prompt_guidance()` with coordinator/team-aware prompt injection
- [x] 9.4 Add worker agent context injection: when spawned agents start, inject context about their role, available tools, and team communication

## 10. Bootstrap Wiring

- [x] 10.1 Update `src/tools/registry/tool-context.hpp` -- add `coordinator_manager`, `team_agents`, `is_child_run` fields (replacing old subagent fields)
- [x] 10.2 Update `src/bootstrap/bootstrap.cpp` -- create `AgentDefinitionRegistry`, `AgentMailbox`, `TeamManager`, `CoordinatorManager` and wire them together
- [x] 10.3 Update `src/bootstrap/agent-runtime.hpp/.cpp` -- pass new fields through `AgentRuntimeBuildInput` to `ToolRuntimeContext`
- [x] 10.4 Update `src/bootstrap/channel-serve.hpp/.cpp` -- inject `CoordinatorManager` into channel conversation runtimes
- [x] 10.5 Update tool registration logic -- conditionally register coordinator tools (when `coordinator_mode` is true) or standard tools (when not)
- [x] 10.6 Update `src/bootstrap/runtime-control.cpp` -- wire `CoordinatorManager` to web server configuration

## 11. Web & Admin API

- [x] 11.1 Update `src/web/web-server.hpp/.cpp` -- replace `subagent_manager_` with `coordinator_manager_`, add `team_manager_`
- [x] 11.2 Update `src/web/admin-routes.cpp` -- expose team list, team members, and agent definitions instead of subagent lists
- [x] 11.3 Update `src/web/chat-routes.cpp` -- pass `CoordinatorManager` when building web runtime bundles
- [x] 11.4 Update `src/web/web-routes.hpp` -- update forward declarations

## 12. Frontend Updates

- [x] 12.1 Update `web/src/components/admin/AgentsPage.tsx` -- replace subagent badges with team/coordinator badges, show team membership
- [x] 12.2 Update `web/src/components/layout/AgentTree.tsx` -- build tree from `team_agents` arrays instead of `subagents`
- [x] 12.3 Update `web/src/api/client.ts` -- update API types to reflect new team/coordinator fields

## 13. Build System & Integration

- [x] 13.1 Update `xmake/targets.lua` -- add `src/coordinator/*.cpp` and `src/swarm/*.cpp` to `orangutan-lib` sources
- [x] 13.2 Update `xmake/tests.lua` -- add test targets for coordinator and swarm tests
- [x] 13.3 Run full build and verify no compilation errors
- [x] 13.4 Run all tests and verify no regressions
- [x] 13.5 Write integration test: coordinator spawns worker, worker completes, coordinator receives notification
- [x] 13.6 Write integration test: team with 2 agents exchanging messages via mailbox
