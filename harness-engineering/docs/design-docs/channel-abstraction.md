# Channel Abstraction

The legacy `orangutan/` had ~3.6 kLoC of QQ-specific code with no extracted abstraction.
Adding Discord, Slack, Telegram, or a generic webhook required rewriting all of it. v2
makes channels **a trait with a capability matrix**, and channel adapters become
independent, optional libraries.

## The Channel Trait

```cpp
// include/oran/channel/channel.hpp — PUBLIC
namespace orangutan::channel {

class Channel {
 public:
  virtual ~Channel() = default;

  // Identity for logs, audit, hook events.
  virtual std::string_view id() const noexcept = 0;
  virtual std::string_view kind() const noexcept = 0;  // "qq", "discord", "slack", ...

  // What this adapter can do. The agent UI layer adapts.
  virtual Capabilities capabilities() const noexcept = 0;

  // Lifecycle.
  virtual async::Awaitable<core::Result<void>> start() = 0;
  virtual async::Awaitable<core::Result<void>> stop()  = 0;

  // Inbound: long-poll-style coroutine; one InboundMessage per resume.
  virtual async::Awaitable<core::Result<InboundMessage>> next_message() = 0;

  // Outbound: deliver one message; respect capabilities (silently downgrade
  // unsupported features, e.g. drop reactions on a channel that has none).
  virtual async::Awaitable<core::Result<DeliveryReceipt>>
  send(OutboundMessage) = 0;
};

}  // namespace orangutan::channel
```

The agent runtime is given a `Channel&` (or a `std::shared_ptr<Channel>`) by the
bootstrap. It does not know which adapter is behind it.

## Inbound / Outbound Envelopes

```cpp
namespace orangutan::channel {

struct InboundMessage {
  std::string channel_id;          // adapter instance id
  std::string conversation_id;     // jid, channel-id, room-id, …
  std::string user_id;             // platform-native id
  std::string display_name;        // for prompt context
  std::vector<Content> content;    // text + attachments + mentions
  std::vector<Reference> replies_to;  // optional thread / quote context
  core::Time received_at;
  Origin origin;                   // origin::channel::<kind>
  Capabilities caps;               // mirrored from Channel::capabilities()
};

struct OutboundMessage {
  std::string conversation_id;
  std::vector<Content> content;
  std::optional<std::string> reply_to_message_id;
  std::optional<std::string> thread_id;
  std::vector<Reaction> reactions;  // adapter may downgrade
  DeliveryHint hint;                // ephemeral? high-priority? user-mention?
};

struct DeliveryReceipt {
  std::string message_id;          // platform-native; opaque
  core::Time accepted_at;
};

}  // namespace orangutan::channel
```

## Capability Matrix

```cpp
namespace orangutan::channel {

struct Capabilities {
  bool text                 = true;   // baseline; always true
  bool rich_text            = false;  // markdown / formatting
  bool attachments_image    = false;
  bool attachments_file     = false;
  bool attachments_audio    = false;
  bool attachments_video    = false;
  bool reactions            = false;
  bool mentions             = false;
  bool threads              = false;
  bool ephemeral_messages   = false;  // (Slack ephemeral, Discord ephemeral, …)
  bool typing_indicator     = false;
  bool message_edit         = false;
  bool message_delete       = false;
  bool reply_quoting        = false;
  std::size_t max_text_bytes = 4 * 1024;
};

}  // namespace orangutan::channel
```

Adapters fill this honestly at construction. The agent runtime uses it to:

- Decide whether to chunk a long message.
- Decide whether to emit a typing indicator while thinking.
- Decide whether to send `tool_use` previews as ephemeral messages.

## Adapter Library Layout

Each adapter is its own xmake target so users compile only what they need:

```
src/oran-channel/         channel.hpp + ChannelManager
src/oran-channel-qq/      QQ adapter
src/oran-channel-discord/ Discord adapter
src/oran-channel-slack/   Slack adapter
src/oran-channel-telegram/ Telegram adapter
src/oran-channel-webhook/ generic webhook adapter
src/oran-channel-ws/      generic websocket adapter (stretch)
```

xmake option toggles:

```lua
option("channel_qq")       set_default(true)
option("channel_discord")  set_default(false)
option("channel_slack")    set_default(false)
option("channel_telegram") set_default(false)
option("channel_webhook")  set_default(true)
```

Disabled adapters do not link, do not compile. The `oran-bootstrap` config-loader
silently ignores config entries for disabled adapters with a single warning.

## ChannelManager

`oran-channel::ChannelManager` is the *one* component the agent runtime knows about:

```cpp
namespace orangutan::channel {

class ChannelManager {
 public:
  explicit ChannelManager(async::Runtime&);

  // Register at startup; adapters are owned by the manager.
  void register_adapter(std::unique_ptr<Channel>);

  // Lifecycle.
  async::Awaitable<core::Result<void>> start_all();
  async::Awaitable<core::Result<void>> stop_all();

  // Fan-in: returns a bounded async::Channel of InboundMessage from all adapters.
  // The receiver is the agent runtime (or oran-orchestration's dispatcher).
  async::Channel<InboundMessage>& inbound();

  // Direct send (when the agent has a specific channel target).
  async::Awaitable<core::Result<DeliveryReceipt>>
  send(std::string_view channel_id, OutboundMessage);

  // Capability lookup.
  Capabilities caps(std::string_view channel_id) const;
};

}  // namespace orangutan::channel
```

The manager:

- Owns lifetimes of all registered adapters.
- Multiplexes inbound queues into a single bounded `async::Channel<InboundMessage>`.
- Provides per-channel send.

## Per-Conversation Serialization

Inside one channel, messages for the same `conversation_id` should be handled in order.
The legacy code's `JidTaskRunner` is the right idea; v2 generalizes it via a
`PerKeyStrand<conversation_id>`:

```cpp
// inside ChannelManager
strands_.for_key(msg.conversation_id, [&]() {
  asio::co_spawn(strand_, dispatcher.handle(std::move(msg)), asio::detached);
});
```

Multiple conversations on the same channel adapter run in parallel; a single
conversation's messages are strictly ordered.

Downside of the legacy approach: one slow response blocked the entire JID queue. v2
mitigates by:

- Per-message deadline (`config.channel.<id>.message_deadline_seconds`).
- On deadline, the in-flight tool calls are cancelled; the agent emits a "still
  working" message and rejoins later.

## QQ Adapter Migration

`oran-channel-qq` reuses the legacy QQ API client logic but **decoupled**:

- All curl handling goes through `oran-http::Client`. No direct `CurlHandle` use.
- OAuth + token refresh lives in `oran-channel-qq::TokenStore`, isolated.
- Media handling becomes a separate file under `oran-channel-qq::Attachments` — and
  *the abstraction* (download URL, get bytes, cache) is shared with other adapters via
  `oran-channel::AttachmentCache`.

`docs/exec-plans/active/<date>-channel-qq-port.md` (to be created) will manage the port.

## New Adapter Recipe

To add a new platform adapter:

1. Open `docs/exec-plans/templates/execution-plan.md`, write a plan.
2. Create `src/oran-channel-<name>/`, `include/oran/channel-<name>/`, `tests/oran-channel-<name>/`,
   `bench/oran-channel-<name>/`.
3. Implement `Channel`, fill `Capabilities` honestly.
4. Add `option("channel_<name>")` in `xmake/options.lua`.
5. Wire into the bootstrap config schema (one new section under `channels:`).
6. Add adapter-specific docs under `docs/design-docs/channel-<name>.md` if the platform
   has noteworthy quirks (e.g. rate limits, OAuth dance).
7. Write at least one integration test that uses a mock HTTP server (`oran-http`'s
   `tests/http/MockServer`).
8. Write a smoke bench: connection setup time, inbound throughput.

## Hook Surface

Channel lifecycle is hookable. See
[`permissions-and-hooks.md`](permissions-and-hooks.md). Events:

- `channel.start`         — adapter started OK.
- `channel.stop`          — adapter stopped (graceful).
- `channel.inbound`       — message received before runtime sees it. Hook can drop the
                            message (rate-limiting), tag it, or rewrite content.
- `channel.outbound.pre`  — before send to platform.
- `channel.outbound.post` — after platform accepts.
- `channel.delivery_error` — send failed; payload includes retry hint.

## Anti-Goals

- **Not** trying to be a generic ESB. Each adapter is goal-built for its platform's
  conversational model.
- **Not** abstracting away platform-specific UX like reactions, ephemeral messages.
  The capability matrix surfaces those rather than pretending all platforms are the same.
- **Not** trying to support websocket adapters as a v1 deliverable; that's stretch.

## See Also

- [`permissions-and-hooks.md`](permissions-and-hooks.md) — every channel checkpoint
  is also a hook point.
- [`../product-specs/0003-multi-platform-channels.md`](../product-specs/0003-multi-platform-channels.md)
  — concrete v1 deliverables.
- Legacy QQ implementation lives in `../../orangutan/src/channel/qq/`. Use as reference,
  not as code to copy.
