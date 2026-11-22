# Configuration And State

The host supplies parsed configuration through `Config::parse` or
`Config::load_file`. Each object is read through a field view: the fields the
parser reads are the recognized fields, and any other key in any object is an
"unknown config field" at its JSON path. Strict loading (`strict_config` or
`LoadOptions`) makes it an error; otherwise it is a warning. The first invalid
value fails the parse with its path; integers are range-checked before they are
narrowed. Parse configuration before constructing provider services.

The config file contains:

```jsonc
{
  "strict_config": true,
  "runtime": {},
  "trace": {},
  "profiles": {},
  "routes": {},
  "permissions": {},
  "hooks": {},
  "memory": {}
}
```

## Settings And Ownership

| Section | Consumer |
| --- | --- |
| `runtime.workers` | Host maps the blocking worker count to `async::RuntimeConfig`. |
| `runtime.request_timeout_ms`, `runtime.stream.max_bytes` | Host maps transport bounds to `HttpProviderBackendOptions`. |
| `runtime.tool_output` | `AgentSession` applies model-visible text and structured output byte limits. |
| `runtime.tool_scheduler` | `AgentSession` applies parallelism and per-call dispatch timeouts. |
| `runtime.prompt.active_tools` | `AgentSession` maps configured names to the loop's native tool selection. |
| `trace.enabled` | Host maps the trace switch to `RuntimeAssemblyOptions`. |
| `profiles`, `routes` | `HttpProviderBackend` selects model, protocol, endpoint, credentials and model policy; `route_name` defaults to `default`. |
| `profiles.<name>.cache` | The selected protocol target applies explicit cache controls; [provider](api-portability.md) owns eligibility and service limits. |
| `permissions` | `AgentSession` materializes configured external-effect rules. Host maps workspace roots to `WorkspaceOptions`; dynamic children inherit the parent boundary. |
| `hooks.timeout_ms` | Host maps the hook deadline to `RuntimeAssemblyOptions`. |
| `memory.longterm.recall` | `AgentSession` resolves automatic index enablement, limit and kinds; an explicit optional `longterm_recall` overrides it. |

Parsed settings stay at the composition boundary. Bootstrap maps them into owned
permission rules and provider profiles through the
[configuration adapters](bootstrap-runtime.md#configuration-adapters). Permission
and provider execution consume those values without retaining configuration views.

`runtime.prompt.active_tools` accepts `"defaults"` or an array of registered tool
names. Defaults expose every registered tool, including host extensions. An
explicit array selects only those names; `[]` exposes none. Unknown names fail
before provider execution. Disabled delegation is removed from both the available
catalogue and explicit selection. [Tools](tool-runtime.md) owns this boundary.
Path locks follow active work and have no idle expiration setting. Child agents
are created from AgentRun tasks; configuration has no named agent presets.

Memory orientation defaults to enabled with a 20-entry limit when the backend
exists. `enabled: false` disables the automatic index. Memory tools use
host-bound scope, functional tool availability and the memory write gate without
generic allow/deny/ask. [Memory](memory-system.md) owns consultation,
index budgeting and correction semantics.

## Credentials

Profiles store an `api_key_env` name. Credential resolution reads the named
variable or an explicitly injected `provider::SecretLookup` at the provider
construction boundary.
Values are excluded from diagnostics. String values the parser reads expand
`${NAME}` and `${NAME:-fallback}`; unread fields are never expanded; prefer references over
embedding credentials in JSON. No general provider credential store is implemented.
Channel hosts own separate credentials: the QQ login command stores a mode-0600,
unencrypted binding outside the workspace. This is not a general provider secret
store; [QR authorization](messaging-channels.md#qr-authorization) owns its format
and preservation rules.

## State

The host selects the workspace and database paths. `RuntimeAssembly` defaults to
`sessions.db`, `memory.db` and `audit.db` under `<workspace>/.orangutan`. The host
owns directory privacy and exclusion; `io::PrivateDirectory` provides private
files and a directory lock.

`AgentSessionOptions` carries the session ID, agent key, approval identity and
memory scope. Reusing a session ID and agent key resumes the stored conversation.
Long-term recall uses the supplied scope; workspace-to-scope mapping is host policy.

No automatic import or unrelated-history merging occurs. Preserve database
files and migration records through refactors. Backup/import tooling remains
explicit work in [live debt](../exec-plans/tech-debt-tracker.md).
