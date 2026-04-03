## ADDED Requirements

### Requirement: Agent definition loading from config
The system SHALL load agent definitions from the `"agents"` section of `config.json`. Each agent definition SHALL support: `profile`, `model`, `fallback_models`, `workspace`, `permissions`, `team_agents`, `coordinator_mode`, `edit_mode`, `thinking_budget`, and optionally `agent_definition` (path to a markdown definition file).

#### Scenario: Config-based agent with coordinator mode
- **WHEN** config contains an agent with `"coordinator_mode": true` and `"team_agents": ["researcher", "coder"]`
- **THEN** the agent SHALL be loaded as a coordinator with access to spawn researcher and coder agents

#### Scenario: Config-based agent without coordinator mode
- **WHEN** config contains an agent without `"coordinator_mode"` or with it set to `false`
- **THEN** the agent SHALL be loaded as a standard agent with the full tool set

### Requirement: Agent definition loading from directory
The system SHALL scan `.orangutan/agents/` under the workspace root for markdown files with YAML frontmatter. Each file defines an agent type with: `description` (when to use this agent), `tools` (allowed tool list), `model` (model override or "inherit"), `max_turns` (optional iteration limit), and the markdown body as the agent's system prompt addendum.

#### Scenario: Load agent from markdown file
- **WHEN** a file `.orangutan/agents/researcher.md` exists with valid frontmatter
- **THEN** the agent definition SHALL be loaded and available for spawning by coordinators and team leads
- **THEN** the `description` field SHALL be used to present the agent's purpose to the LLM

#### Scenario: Invalid frontmatter
- **WHEN** an agent definition file has missing required fields (e.g., no `description`)
- **THEN** the system SHALL log a warning and skip the invalid definition

### Requirement: Agent definition frontmatter schema
Agent markdown definition files SHALL use the following YAML frontmatter schema:
```yaml
description: string (required) -- when to use this agent
tools: string[] (optional) -- allowed tools; if omitted, inherits full set
disallowed_tools: string[] (optional) -- tools to exclude from inherited set
model: string (optional) -- model override or "inherit"
max_turns: number (optional) -- maximum agent loop iterations
```

#### Scenario: Agent with restricted tool set
- **WHEN** an agent definition specifies `tools: ["bash", "read", "grep"]`
- **THEN** the agent SHALL only have access to those three tools when spawned

#### Scenario: Agent with disallowed tools
- **WHEN** an agent definition specifies `disallowed_tools: ["bash"]`
- **THEN** the agent SHALL have access to all standard tools except bash

### Requirement: Built-in agent definitions
The system SHALL provide built-in agent definitions that are always available: `general-purpose` (full tool access, standard prompt), `explorer` (read-only tools for codebase exploration), and `planner` (read-only tools with planning-focused prompt). Built-in agents SHALL be overridable by config or directory definitions with the same name.

#### Scenario: Built-in agents available without configuration
- **WHEN** no custom agent definitions are configured
- **THEN** the `general-purpose`, `explorer`, and `planner` agent types SHALL be available for coordinator spawning

#### Scenario: Custom definition overrides built-in
- **WHEN** a file `.orangutan/agents/explorer.md` exists
- **THEN** the custom definition SHALL take precedence over the built-in `explorer` definition

### Requirement: Agent definition registry
The system SHALL maintain an `AgentDefinitionRegistry` that merges built-in, config-based, and directory-based definitions. The registry SHALL be queryable by agent key and SHALL return the resolved definition including tools, model, prompt, and constraints.

#### Scenario: Query agent definition
- **WHEN** the registry is queried for agent key "researcher"
- **THEN** it SHALL return the resolved definition or `nullopt` if not found

#### Scenario: Definition precedence
- **WHEN** an agent key exists in both config and directory
- **THEN** the directory definition SHALL take precedence for fields it specifies
- **THEN** config fields not overridden by the directory definition SHALL be preserved
