# Orangutan

A C++26 agent runtime for applications that need model calls, permission-checked
tools and persistent conversation memory.

The libraries run agent turns through Anthropic Messages or OpenAI Responses,
save completed turns atomically, and delegate bounded tasks to dynamically
created child agents. Children keep independent conversations and inherit
external-effect restrictions; see the
[agent contract](docs/design-docs/agent-platform.md).

## Build

Requires Linux, GCC 16.1 or later, xmake 3.1.1 and system libcurl development files.
Xmake resolves the remaining [dependencies](docs/rules/libraries.md).

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4
```

## Desktop Prototype

The optional [desktop browser prototype](apps/desktop/README.md) provides a
five-page assistant workspace with bilingual light/dark themes, local demo
execution and read-only views of channel conversations. It uses simulated data
and is independent of the C++ runtime. Its README contains setup and verification
commands for Node.js 24 or later.

## Embed

Link `oran-bootstrap` and include `<oran/bootstrap.hpp>`. Compose the runtime
through three interfaces:

| Interface | Responsibility |
| --- | --- |
| `HttpProviderBackend` | Construct a provider route and HTTP transport from explicit configuration. |
| `RuntimeAssembly` | Own workspace, permissions, hooks and storage services. |
| `AgentSession` | Run prompts, recall scoped memory, dispatch tools and persist completed turns. |

The host supplies executors, workspace, session identity and resource lifetimes.
Await `AgentSession::run_prompt` before releasing its services. The
[composition contract](docs/design-docs/bootstrap-runtime.md) describes ownership;
the [HTTP continuation test](tests/bootstrap/test_provider_backend.cpp) exercises
the complete setup against a controlled provider.

[config.example.json](config.example.json) shows provider and permission settings.
Real providers require the named API-key environment variable; the test suite
uses local fixtures. See [configuration](docs/design-docs/secrets-and-state.md).

Tool calls share input validation and observations. External tool effects require
workspace authority and permission; an ask fails closed without an approval handler.
Internal memory and coordination use host-bound scope and functional admission,
so routine bookkeeping does not require tool approval.
The filesystem tools are FileRead, FileWrite and FileEdit. `AgentRun` creates a
fresh child from `{"prompt":"Inspect the change"}` without preset configuration.
It inherits the parent's model, workspace, scope and external-effect limits, with
up to four child runs per prompt and one generation of delegation.

QQ, Telegram and Feishu text integrations use SDK-free adapters, injected session
and transport ports, and joined typing-status cleanup. See the
[channel hosting contract](docs/design-docs/messaging-channels.md) for authenticated
ingress, credentials, permissions and delivery recovery.
For real Telegram testing, build the opt-in `oran-telegram` host and follow the
[private-chat setup](docs/design-docs/messaging-channels.md#telegram-deployment-host).
It includes a DeepSeek Flash example, persistent conversations, status reactions,
streaming previews, Markdown formatting, reply context and image input for vision-capable models.
Chat commands `/new`, `/status`, `/help` and `/whoami` provide local session controls.
The retained host also supports background child tasks: AgentRun can return an
immediate receipt while later messages continue, and completed work produces a
reviewed follow-up. `/tasks` lists progress and `/stop` requests cancellation.
Task execution is process-local; `--once` does not enable background work.
The retained host also enables [agent teams](docs/design-docs/agent-teams.md): the
leader defines roles as needed, assigns dependent tasks, reviews results and
continues the same specialists for corrections. Members exchange targeted
messages. Library hosts enable this with `BackgroundTaskOptions::enable_teams`.
For QQ, `oran-qq-login` supports [official QR binding](docs/design-docs/messaging-channels.md#qr-authorization)
without manually entering AppID/AppSecret, plus a saved-credential probe. The QQ
message-receiving host is still separate deployment work.

Working-context retention can be measured with the opt-in `eval-context` runner.
See [evaluation commands and interpretation](docs/rules/testing-and-bench.md#working-context-evaluation).

## Development

Read [CLAUDE.md](CLAUDE.md) and [STATUS.md](docs/STATUS.md). Run `make ci` alongside
C++ build/tests before committing. [Architecture](docs/ARCHITECTURE.md) owns
module boundaries; [live debt](docs/exec-plans/tech-debt-tracker.md) records
remaining reliability and verification work.

No redistribution license is currently provided in this repository.
