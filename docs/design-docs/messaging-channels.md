# Messaging Channels

`oran-channel` integrates Telegram Bot API, official QQ Bot API and Feishu text
messages. It depends on existing core, async, permission and hook libraries, with
JSON definitions private to its implementation. No messaging SDK or new package
is required. `<oran/channel.hpp>` exposes the host boundary.

## Ports and protocol strategies

`Adapter` is a stateless strategy for authenticated envelope decoding, reply and
activity request construction, capabilities and response validation. The
`telegram()`, `qq()` and `feishu()` factories return process-lifetime strategies.
New protocols implement this same interface. Pure conversion does not open
connections, read configuration, resolve secrets or start agent turns.

`Transport` accepts a conversation and credential-free relative request.
`RunTurn` accepts a normalized message and returns the completed answer. A
`Dispatcher` owns admission, deduplication, delivery progress and activity work.
It borrows its adapter and hook bus and owns its callback values. It has no
provider, session, HTTP, configuration or storage dependency.
An optional pure `DispatcherOptions::render_reply` supplies host formatting
before reply preflight and authorization; rules inspect the final request body.
Without it, the dispatcher uses the adapter's ordinary text reply conversion.
An optional pure `split_reply` preserves host formatting across parts. The
dispatcher rejects empty, invalid UTF-8 or oversized parts before sending; all
parts still pass adapter preflight and the ordinary send authorization.

Conversation identity includes platform, configured account, direct/group kind,
chat ID and optional thread ID, with length-delimited components. The account
and bot identity come from trusted host configuration. `conversation_key` is a
host session-routing key; map it to one retained `AgentSession` and a durable
session ID. Choose memory scope and sender authority explicitly. A group shares
its conversation; use sender rules if group members have different authority.

| Platform | Accepted ingress | Reply | Activity |
| --- | --- | --- | --- |
| Telegram | User text, photos and image documents with captions in private/group/supergroup chats | `sendMessage`, original message and forum topic | `sendChatAction`, refreshed every four seconds by default; expires naturally |
| QQ | `C2C_MESSAGE_CREATE`, `GROUP_AT_MESSAGE_CREATE` text or image dispatch envelopes, with reference indices | Official `/v2/users` or `/v2/groups` passive text reply; opt-in native Markdown | C2C `msg_type: 6`, `input_type: 1`, then explicit `input_type: 2` stop; group activity unsupported |
| Feishu | `im.message.receive_v1` user text events | Message reply with nested JSON content and thread routing | Add `Typing` reaction, then delete the exact returned reaction ID |

Bot/self events where the protocol identifies bots, edits, unsupported attachments
and other unsupported event types are ignored; malformed supported events return errors.
Only configured bot mentions are removed, never arbitrary user mentions.
Telegram group mention/command policy belongs to the host admission rules.
Feishu requires the bot open ID to strip its mention placeholders.

Plain text is sent without Telegram Markdown parsing. UTF-8 is validated and
split at code point boundaries using conservative byte limits (Telegram/Feishu
4000, QQ 2000). Empty answers produce no message. QQ replies carry the incoming
`msg_id` and increasing `msg_seq`; direct activity reserves sequences 1 and 2,
so direct reply sequences start at 3, group sequences at 1. Reply preflight
rejects more than five QQ chunks before sending any of them. QQ passive-reply
windows and platform quotas still apply; hosts must reconcile expired replies.
Media uploads, cards, editing streamed drafts and QQ guild channels are outside
this reply contract. QQ input preserves the first image attachment, including
image-only messages; the host downloads it after admission. Bot events are ignored.
`Message::reference_key` and `ReplyContext::reference_key` preserve Tencent's
`msg_idx`/`ref_msg_idx` values; quoted message type 103 also supplies `msg_idx`
through `msg_elements`. Authenticated inline `msg_elements.content` supplies
quoted text even when no local index entry exists. These indices are not message IDs. QQ input-status success may be an empty object; only actual message delivery
requires a nonempty receipt ID. Successful QQ message receipts preserve
`ext_info.ref_idx` separately so a host can durably index the exact outgoing part.
Reference resolution must stay within the admitted account/conversation. Missing
references remain explicitly unavailable rather than borrowing nearby history.

`qq_markdown_reply` constructs native `msg_type: 2` / `markdown.content` while
retaining passive reply identity and sequence rules. The account must support
native Markdown; callers select ordinary text explicitly when it does not.
The [QQ live evaluator](../rules/testing-and-bench.md#qq-live-dialogue-evaluation)
composes these features, bounded image loading and local commands.

## Hosting and authentication

The application owns ingress servers/polling/gateway connections, signature
verification, replay windows, acknowledgments and durable intake. `decode` is
**not an authentication API**. Call it only after checking the configured account's
Telegram webhook secret header (or receiving authenticated polling results),
QQ callback signature/gateway session, or Feishu signature, verification token
and optional encrypted envelope. Handle webhook verification challenges at that
same host boundary. Acknowledge intake promptly after durable enqueueing; never
wait for an agent turn inside a webhook acknowledgment deadline.

Bootstrap's `channel_http_transport(Client&, ChannelCredential)` binds outbound
requests to the existing HTTP client and fixed official HTTPS endpoints. Supported
Telegram methods cover sends, typing, reactions, ephemeral drafts and file metadata;
arbitrary API paths remain rejected. The binding uses a ten-second request timeout
and a one-MiB response cap. The credential callback
returns a current Telegram bot token, QQ access token, or Feishu tenant access
token on each request. The host owns environment/secret references and token
refresh (QQ app credentials and Feishu app credentials are not access tokens).
The callback must be cancel-aware and finish borrowed work before returning.
Credentials enter only the HTTP binding, never requests, hooks or diagnostics.
Upstream bodies and credential-callback error text are not propagated as errors.
No channel settings are implicitly read from the runtime configuration.

QQ permits only POST to `/v2/users/{id}/messages` and `/v2/groups/{id}/messages`.
Its configured account identity is the AppID, also sent in `X-Union-Appid`.
Feishu permits POST reply/reaction creation and DELETE of one reaction. Identifiers
must remain single path segments even after percent decoding. Unsupported methods,
arbitrary API suffixes and encoded path traversal fail before credential lookup.

A host composes these ports on its coordinating strand:

```cpp
channel::DispatcherOptions options;
options.hooks = &hooks;
options.rules = std::move(channel_rules); // allow ChannelReceive/Send/Typing as needed

auto dispatcher = channel::Dispatcher::create(
    channel::telegram(),
    bootstrap::channel_http_transport(http_client, current_credential),
    [&session](channel::Message message) -> async::Awaitable<core::Result<std::string>> {
      auto result = co_await session.run_prompt({.prompt = std::move(message.text)});
      if (!result) co_return std::unexpected(result.error());
      co_return std::move(result->text);
    },
    std::move(options));
```

This text-only example binds one conversation. Image attachments also need the
host's authorized download and typed prompt mapping. A multi-conversation host
resolves its session by `conversation_key` inside the turn callback. The host supplies the
HTTP client's worker executor and retains all services until every dispatch has
returned. The bootstrap channel integration test exercises a real `AgentSession`
with a controlled provider and transport.

## Telegram deployment host

The opt-in `oran-telegram` executable composes polling, the dispatcher and one
persistent `AgentSession`. It accepts only the explicitly configured user's
private chat. Groups, other senders and unsupported updates are skipped. It
processes one update at a time, keeps Telegram's pending queue intact on startup,
verifies `getMe`, and refuses an active webhook without changing it. Run only one
poller per bot token. The state lock excludes another process using the same
state directory; Telegram rejects competing pollers in different directories.

Photos select the largest available size; image documents keep their Telegram
file ID and caption. After receive admission, `ChannelAttachment` authorizes both
`getFile` and the bounded download from Telegram's fixed file endpoint. The host
accepts JPEG, PNG, GIF and WebP signatures up to 5 MiB, with a 20-second download
timeout. Neither bot tokens nor download URLs enter provider image blocks or
session history. Captions accompany the image; without one, the host asks the
model to inspect it in conversation context. Album entries remain separate turns.
Unsupported or failed downloads receive a visible explanation rather than
silently disappearing. The configured DeepSeek Flash model supports vision
through its [Anthropic-compatible API](https://api-docs.deepseek.com/guides/anthropic_api).

One level of `reply_to_message` retains the original text/caption, message and
sender IDs, and an optional image, including replies to the bot's own messages.
`quote.text` is retained verbatim as the selected passage, independently of the
full original message; the host does not reconstruct it from UTF-16 offsets.
Available external-reply image metadata and quote-only replies also work without
inventing unavailable original text. Nested reply chains are not expanded.
The prompt wraps reply context and the current request in distinct JSON fields
inside the user message. Quoted text remains reference data and never becomes
system instructions. Attached images are numbered: the replied image first,
then the new image, each downloaded through the same permission and size gates.
Text-only messages keep their original prompt representation. This framing follows
the provenance distinction in the [reference prompt](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-prompt-project-timeline-user-message-provenance.md).

```sh
xmake build -j4 oran-telegram
# Inject ORAN_TELEGRAM_TOKEN and ORAN_TELEGRAM_MODEL_KEY into the environment.
# Supply the allowed user and host-selected workspace/state directories.
build/linux/x86_64/release/oran-telegram \
  --config apps/telegram/deepseek.example.json --allow-user "$TELEGRAM_USER_ID" \
  --workspace "$WORKSPACE_DIR" --state "$STATE_DIR"
```

`--token-env NAME` selects a different bot-token reference. `--probe --state DIR`
checks identity/webhook status without receiving messages or invoking a model;
it needs only the Telegram credential. `--once` processes one admitted message
and exits without advertising background tasks. SIGINT/SIGTERM cancels and joins
active work. Polls use a 25-second long-poll timeout, shortened to two seconds
while background tasks or completions are pending, a 35-second HTTP bound and a
one-MiB response limit. Retryable
poll failures retry at most four times and respect Telegram rate-limit delays.
Transient server failures are retryable; competing pollers return a terminal
conflict. Transport retry metadata survives error redaction.
Sends and agent turns are never retried automatically by the host.

The DeepSeek example uses the Anthropic-compatible endpoint and the
`ORAN_TELEGRAM_MODEL_KEY` reference, with FileRead, MemoryRecall, MemoryRemember,
AgentRun, TaskGet, TaskCancel and the [team tools](agent-teams.md#tools-and-task-state)
selected. It permits provider requests. Memory
and delegation use their functional runtime boundaries. Filesystem writes are not
exposed by this example; other configurations retain normal session permission
decisions. The host provides no interactive approval consumer.
The model receives typed text and image blocks. Local host commands are handled
before any model call or attachment download.

### Chat commands

| Command | Result |
| --- | --- |
| `/new` | Persist a fresh session ID and acknowledge it without calling the model. The next prompt uses a new AgentSession. Existing transcript/checkpoint rows and conversation-scoped long-term notes remain intact. |
| `/status` | Show the active session ID, service uptime, configured model, saved message count, summary coverage and the most recent traced model/token usage for this session. Missing or disabled statistics are explicit. |
| `/tasks` | List retained tasks in the current session with their labels, authoritative state and elapsed time, without a model call. |
| `/stop` | Request cancellation of the current session's background tasks and suppress their pending automatic notifications. Running cleanup stays visible as cancelling; results remain inspectable. |
| `/help` | Show the supported command catalogue and input guidance. `/start` and `/commands` are aliases; `/start` accepts Telegram's onboarding payload. |
| `/whoami` | Show the admitted Telegram user and chat IDs. |

Commands must be sent alone; unexpected arguments do not execute them. Command
names and this bot's `@username` suffix are case-insensitive. Commands addressed
to another bot are skipped. Only the current text is parsed: quoted text and
image captions never execute commands. Embedded mentions of commands and tokens
containing path separators remain ordinary input. Unknown standalone commands
return help guidance without invoking the model.

After bot/webhook/binding checks, `ChannelMenu` authorizes `setMyCommands` for a
menu scoped to the allowed private chat. Failure is advisory; typed commands
remain available. Probe mode never changes the menu. Command replies use the
same receive/send gates, joined presentation and durable delivery journal as
model replies; commands themselves are not model transcript entries.

`/new` requests cancellation of the old session's background tasks before switching,
then saves the new identity and confirmation together in the pending journal
before sending. An ambiguous confirmation still requires normal reconciliation;
restart never rotates the session again. The cached AgentSession is replaced
at the next model prompt. `/status` requires `ChannelInspect` and reads bounded
metadata and one trace record on the worker executor, without exposing transcript
contents, credential references or filesystem paths. Token figures describe the
last recorded turn, not a live context occupancy estimate.

### Background completion

The retained host binds bootstrap's [background task service](bootstrap-runtime.md#background-tasks)
when at least two blocking workers are configured. No agent presets are needed.
It also enables dynamic teams on that same service. The leader may create roles,
add members and coordinate reviewed tasks; peers may exchange bounded messages.
It caps running background children at `min(4, workers - 1)`, leaving capacity
for foreground provider requests and channel polling because HTTP calls occupy
blocking workers. `--once`, a one-worker Telegram host and the one-event QQ
evaluator do not bind it. AgentRun can acknowledge an accepted task and release the foreground turn;
later user turns continue while the child runs. Native TaskGet/TaskCancel and
host task controls use owner matching and channel observations without generic tool approval.

Polling gives real user updates priority. An empty poll can dispatch one pending
completion through `AgentSession::run_completion`, which consolidates up to four
ready reports into one parent continuation. A completion is runtime evidence,
never a fabricated user request or approval. Its deterministic event key is
`background-<task-id>`. There is no source Telegram message, so the host sends no
reaction, draft or reply reference for that event; ordinary typing and the final
answer use the channel dispatcher. Child tool events do not update foreground
presentation. `/tasks` obtains a fresh scoped snapshot instead of replaying events.

Before invoking the parent, the journal records the anchor `task_id`. It then
uses the same answer-before-send and per-part receipt path as ordinary replies.
The parent continuation commits before the in-process completion acknowledgment;
external delivery settles separately. Failed turns or ambiguous sends retain the
journal for explicit reconciliation, without rerunning child work. Background
delivery never advances the Telegram update cursor. Task jobs and completion
claims do not survive process restart; child transcripts and saved pending
answers do. Unavailable old task IDs must not be presented as still running.

The host polls serially, so commands wait behind an active turn. This command
surface does not yet interrupt a running turn, change models or force compaction.
Local handling, a native menu and model-free new-session acknowledgments follow
OpenClaw's [slash commands](https://github.com/openclaw/openclaw/blob/main/docs/tools/slash-commands.md)
and [reset handler](https://github.com/openclaw/openclaw/blob/main/src/auto-reply/reply/commands-reset.ts).

### Presentation and persistence

The host renders CommonMark with `cmark`, then sends plain text plus Telegram
native entities. Supported formatting includes emphasis, headings, links, lists,
quotes, inline code and fenced code with language labels. Raw HTML is literal
text; image syntax produces a link, not a media upload. GFM tables, strikethrough
and Telegram custom emoji/sticker payloads are outside this CommonMark surface.
Entity offsets count UTF-16 code units, including emoji. Long answers split at
UTF-8 boundaries and clip/rebase entities per part; a code block retains its
format across messages. Telegram's incompatible code/style overlaps are removed
without dropping text. The journal retains the original Markdown answer.

Provider deltas update an in-memory preview; one joined worker serializes status
and preview requests at one-second intervals. `sendMessageDraft` displays the
latest text page without creating a permanent message. Drafts refresh after
15 seconds while active; Telegram expires them after 30 seconds. Before returning
the answer, the admitted turn stops preview scheduling and joins in-flight draft
requests. Only then may the normal journaled `sendMessage` delivery begin: a late
draft must never reappear after the final message. Reasoning text and tool
arguments never enter previews. Provider retries and tool continuations reset
the preview buffer. Clients that do not show drafts still receive final replies.

After `ChannelReceive` admits the turn, the host uses Telegram-supported reactions:
👀 received, 🤔 thinking, 👨‍💻 tool activity, ✍ answering, 👍 delivered and 😱 failed
or cancelled. Short intermediate states may coalesce. Terminal reactions follow
delivery and remain on the original user message. `ChannelStatus` and
`ChannelDraft` each require `egress_http` and publish a channel hook decision.
These advisory failures do not replace the answer. Two failures disable the
affected feature for that turn, and rate-limit cooldown survives subsequent
turns. Shutdown joins any in-flight feedback before emitting the terminal state
and releasing session resources. Existing typing activity remains independent.

Keep state and credentials outside the agent workspace and its extra filesystem
roots. The host requires a private, locked state directory and rejects a workspace
or extra root containing that directory. It maps configured worker/HTTP bounds,
trace settings, workspace roots and hook timeouts into bootstrap. State binds the
bot ID, allowed user, canonical workspace and stable session ID. Reusing it resumes
conversation history and scoped memory; changing an identity requires a different
state directory. Credentials never appear in journal metadata or console output.

`state.json` records the next update and a pending intake before any turn starts.
Journal JSON is validated at load into typed identity, cursor and pending values;
the on-disk format and saved session identity remain unchanged. Reconciliation
rejects a pending update older than the cursor before writing an archive.
The host saves the generated answer before sending, marks in-flight sends, and
records confirmed chunks before advancing the cursor. An interrupted or failed
turn/send stops the host and leaves the pending record intact. Restart refuses to
replay it, including the crash window between sending and saving a receipt.
The session database retains successful turns independently of delivery.

Inspect the pending update, answer and confirmed chunk count, reconcile any
ambiguous delivery in Telegram, then explicitly retire that exact update:

```sh
build/linux/x86_64/release/oran-telegram --state "$STATE_DIR" --ack-pending "$UPDATE_ID"
```

This archives the complete journal as `handled-<update-id>.json` before advancing the
cursor. For a pending background notification, use its recorded `update_id`;
the archive is `handled-task-<task-id>.json` and the cursor stays unchanged.
It does not rerun the model or resend an answer. Preserve the archive
when manually handling an unsent response. It needs no credentials and refuses
a running host or a mismatched update ID. The host favors recoverable intake and
explicit reconciliation; it does not promise exactly-once remote delivery.

## Official QQ webhook and credentials

`<oran/bootstrap/qq.hpp>` supplies the official callback and credential boundaries.
It does not start a public HTTP server or a WebSocket gateway.
The embedding host supplies TLS ingress, secret references, a durable inbox and
outbox, and workers that feed the existing QQ dispatcher/session loop.

`verify_qq_webhook` checks the original body bytes with QQ's Ed25519 scheme:
repeat the app secret to obtain a 32-byte seed and verify timestamp + raw body
against `X-Signature-Ed25519`. Pass `X-Signature-Timestamp` unchanged and an
explicit current UTC time. Bodies are limited to 1 MiB and timestamp skew to
five minutes. Modified bodies, malformed signatures and stale/future requests
fail before parsing or effects. Signature checks include Tencent's published
test vector. This authenticates ingress; it does not replace sender permissions.

`accept_qq_webhook` handles signed URL validation (op 13) and heartbeat (op 1).
It decodes supported dispatch events only after verification, then authorizes
the unscoped `QQInbox` operation; ingress rules can inspect conversation, sender and event ID.
The injected `QQEnqueue` must durably and idempotently record accepted messages
before returning success. Only then does the callback return `{"op":12,"d":0}`.
Enqueue failure returns `d:1` for QQ retry. Cancellation propagates; authentication,
parse and permission errors remain errors for the HTTP host to map to a non-success
status. Unknown authenticated event types are acknowledged without enqueueing.
The HTTP host must enforce the body limit while receiving, before allocating the
complete body. Retries within the timestamp window require durable event-ID
deduplication; this API does not claim exactly-once delivery.

`QQTokenSource` owns one account's resolved app secret and cached access token.
Bind its `get(Conversation)` to `channel_http_transport`; the account must match
the source's AppID. Inject the ordinary HTTP client through `QQHttpSend` and retain
the source/client/hook bus until all callers finish. Refresh uses the fixed
`https://bots.qq.com/app/getAppAccessToken` endpoint, a ten-second timeout and a
16-KiB response bound. `QQToken` requires `egress_http` and publishes its decision
before the request. A cached token introduces no network request.

Calls run on one coordinating strand. One refresh holds a cancel-aware permit;
up to 32 callers may wait or own it. A cancelled waiter cannot cancel the owner.
The cache accepts positive lifetimes up to 24 hours, with a refresh margin of ten
percent capped at 30 seconds, measured using an injectable monotonic clock.
Responses and exceptions never appear in errors; app secrets and token cache
storage are cleared when the source is destroyed. Failed sends are not replayed
automatically by this credential helper.

Deployment still needs official QQ bot credentials, enabled C2C/group events,
a public HTTPS callback and the platform's egress IP allowlist. There is no
standalone `oran-qq` deployment executable in this slice. Controlled tests cover
signed durable intake followed by real AgentSession/Dispatcher replies, duplicate
delivery and reopening a persistent QQ session; they do not establish live
account approval or network reachability. The opt-in
[QQ dialogue evaluator](../rules/testing-and-bench.md#qq-live-dialogue-evaluation)
can process a trusted private event through a real session and outbound transport.

The wire contracts follow Tencent's [SDK overview](https://github.com/tencent-connect/botgo/blob/master/README.md),
[webhook implementation](https://github.com/tencent-connect/botgo/blob/master/interaction/webhook/webhook.go),
[signature scheme](https://github.com/tencent-connect/botgo/blob/master/interaction/signature/interaction.go)
and [token source](https://github.com/tencent-connect/botgo/blob/master/token/token_source.go).

### QR authorization

`connect_qq` in `<oran/bootstrap/qq_connect.hpp>` obtains bot credentials through
Tencent's official QR binding service. It creates a random 32-byte key, posts its
base64 representation to `https://q.qq.com/lite/create_bind_task`, and passes the
official QQ connect URL to the host's display callback. Every create/poll requires
`QQBind` with `egress_http` and emits a channel hook containing only the operation
and decision. The binding key is never part of the QR URL, hooks or diagnostics.
Retain the options, HTTP client and hook bus until the operation completes.

The operation polls `/lite/poll_bind_result` at two-second intervals by default.
States 0/1 wait, 2 completes, and 3 refreshes the QR with a new key; unknown states
fail. At most three tasks are created under one five-minute deadline (configurable
up to ten minutes). Each HTTP request has at most ten seconds and a 16-KiB response
limit. Request failures stop the operation; it does not retry ambiguous creates.
Cancellation joins HTTP/display work. The service returns an AES-256-GCM envelope
(12-byte nonce, ciphertext, 16-byte tag); authenticate it before returning AppID,
AppSecret and the optional scanning user's open ID. Temporary key storage is
cleared at completion. Existing libsodium requires hardware AES-GCM support;
unsupported machines fail before any binding request.

The opt-in `oran-qq-login` executable displays the QR directly in the terminal,
so no AppID/AppSecret copying is needed:

```sh
xmake build -j4 oran-qq-login
build/linux/x86_64/release/oran-qq-login --state "$QQ_STATE_DIR" --workspace "$WORKSPACE_DIR"
build/linux/x86_64/release/oran-qq-login --state "$QQ_STATE_DIR" --workspace "$WORKSPACE_DIR" --probe
```

Install system `libqrencode` 4.1.1 development files to build this executable.
Use mobile QQ to scan and approve the displayed official page. SIGINT/SIGTERM
cancels and joins the login. `QQCredentialRead`/`QQCredentialWrite` authorize
private storage. The host holds a directory lock and atomically saves
`qq-credentials.json` with `app_id`, `app_secret` and `user_openid`, mode 0600.
The file contains an unencrypted secret: keep the private state directory outside
the agent workspace and every extra filesystem root. The executable rejects state
under its selected workspace and never replaces an existing binding, even with a
malformed file or a new scan. Use a different private directory for another bot.

`--probe` reads that binding and obtains an access token through `QQTokenSource`,
without creating a QR, invoking a model or sending a chat message. Binding itself
only persists credentials; neither command starts a message receiver. A QQ host
must consume the saved identity, select allowed senders (the scanning user's open
ID when available), and compose authenticated ingress, durable intake and delivery.
An absent open ID does not grant access to every user.

The protocol follows Tencent's [official plugin login](https://github.com/tencent-connect/openclaw-qqbot/blob/a730701d36aa7a070f98d4cba0f340f91f15e5f5/src/setup/login.ts)
and the published [`qqbot-connector` 1.2.0](https://www.npmjs.com/package/@tencent-connect/qqbot-connector/v/1.2.0)
wire implementation. That JavaScript package is not a dependency. Controlled
tests verify state transitions, authenticated decryption, denial, cancellation
and private persistence; actual account binding still requires a user's scan.

## Dispatcher authority, lifecycle and recovery

`ChannelReceive` must be allowed before invoking the turn callback. `ChannelSend`
and `ChannelTyping` require `egress_http` for every request. Rules see normalized
conversation/sender and, for HTTP effects, method/path/body. Unmatched strict
rules and `ask` fail closed. `channel_action` observes the decision before the
effect, exposing only platform, account, operation and allowed status. The
session independently authorizes provider/tool effects through its own policy.

The host calls all dispatcher methods on one strand. Same-conversation overlap
returns `conflict`; total admission and retained events are bounded (default
128, maximum 4096). The host queues and retries admission without dropping the
input. HTTP API failures are checked even when HTTP status is 200; rate-limit
delays from Telegram and numeric `Retry-After` headers are preserved. Sends are
never automatically retried: a timeout can mean the message was delivered.

Activity is advisory and never replaces the agent's result. Starts do not
overlap; renewals stop after two consecutive failures, immediately on rate limit
or permission denial, and at the configured TTL (default/maximum 60 seconds).
QQ direct status and Feishu reactions start once. On completion, failure or
cancellation, the dispatcher stops scheduling, joins any in-flight start and
awaits cleanup. It never releases borrowed services while activity work remains.
Cleanup failures are counted on successful `Delivery` results. Remote outages
can prevent Feishu reaction removal; joined cleanup proves lifetime safety, not
remote success. Telegram typing expires without an explicit stop API.

Delivered events are deduplicated in a bounded FIFO cache. A failed turn is not
rerun on duplicate delivery; a generated answer and confirmed chunk offset stay
in memory after a send failure. `pending` exposes them for host persistence and
reconciliation. `resume` explicitly starts at a host-confirmed unsent offset and
never reruns the agent. `acknowledge_failure` makes a terminal failure evictable
only after the host has handled it durably. Pending failures otherwise apply
backpressure and are never silently evicted. Cache eviction or process restart
ends this in-memory deduplication window: durable intake/outbox and restart
recovery belong to the host, not an exactly-once promise from this library.

## References

The interface split and lifecycle safeguards were informed by OpenClaw's
[channel plugin contract](https://github.com/openclaw/openclaw/blob/main/src/channels/plugins/types.plugin.ts),
[typing controller](https://github.com/openclaw/openclaw/blob/main/src/channels/typing.ts)
and [Feishu typing adapter](https://github.com/openclaw/openclaw/blob/main/extensions/feishu/src/typing.ts).
The Telegram host's coalesced reactions and serialized previews also draw on
OpenClaw's [status controller](https://github.com/openclaw/openclaw/blob/main/src/channels/status-reactions.ts),
[Telegram reaction variants](https://github.com/openclaw/openclaw/blob/main/extensions/telegram/src/status-reaction-variants.ts)
and [preview lifecycle](https://github.com/openclaw/openclaw/blob/main/extensions/telegram/src/draft-stream.ts).
The host uses Telegram's native ephemeral drafts, so preview sends do not need
the persistent-message edit/reconciliation machinery used by that implementation.
Wire details follow [Telegram Bot API](https://core.telegram.org/bots/api),
[Tencent's official message types](https://github.com/tencent-connect/botgo/blob/master/dto/message_create.go)
and [Feishu message API](https://open.feishu.cn/document/server-docs/im-v1/message/reply).
These references informed the design; their SDKs and source implementations are
not dependencies. Controlled tests validate the supported wire contract and
lifecycle. Live account permissions and platform acceptance require deployment
validation with the configured bot accounts.
