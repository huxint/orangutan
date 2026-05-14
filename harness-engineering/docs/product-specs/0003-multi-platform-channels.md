# 0003 — Multi-Platform Channels

## User Problem

Operators want to interact with their Orangutan agent from where they already work:
team chat, mobile messaging, custom HTTP integrations. The legacy `orangutan/` had QQ
only — v2 makes channels pluggable.

## Scope (v1)

- `oran-channel::Channel` trait + `Capabilities` matrix.
- `oran-channel::ChannelManager` with per-conversation strands and a bounded inbound
  fan-in queue.
- Two adapters shipping:
  - **QQ** (`oran-channel-qq`) — ported from legacy, refactored onto the trait.
  - **Webhook** (`oran-channel-webhook`) — generic inbound HTTP receiver + outbound
    POST.
- xmake options to disable any adapter (`--channel_qq=n`, etc.).
- Per-channel config in `config.channels[]`.

## Scope (v1.1)

- **Discord** (`oran-channel-discord`).
- **Slack** (`oran-channel-slack`).
- **Telegram** (`oran-channel-telegram`).

## Scope (v2 stretch)

- **WebSocket** (`oran-channel-ws`) — generic, bidirectional.
- **Email** (`oran-channel-email`) — read/reply via IMAP/SMTP.
- **Matrix** (`oran-channel-matrix`).

## Out Of Scope

- Voice channels.
- Native mobile app SDKs.

## Acceptance Criteria

1. Compiling with all v1 adapters disabled produces a binary with zero channel code
   linked in.
2. A QQ message arrives → routed to the configured agent → response sent back; the
   round-trip is observed via the `audit.db`.
3. A webhook adapter accepts a POST body, the agent processes it, the response is
   POSTed back to the configured callback URL.
4. The capability matrix is honored: an adapter without `reactions` capability never
   crashes when the agent tries to react; the reaction is silently dropped + logged.
5. Per-conversation ordering holds: two messages on the same conversation are
   processed in arrival order; messages on different conversations on the same
   adapter run in parallel.
6. Per-message deadline is enforced: a tool call exceeding the configured deadline
   is cancelled, and the agent emits a follow-up "still working" message.
7. `tests/channel/` ≥ 70% coverage including the webhook adapter end-to-end with a
   mock HTTP server.
8. `bench/channel/` reports inbound throughput ≥ 200 msg/s on the mock adapter.

## Design Doc Cross-References

- [`../design-docs/channel-abstraction.md`](../design-docs/channel-abstraction.md)
- [`../design-docs/async-model.md`](../design-docs/async-model.md) (strands, channels)

## Risks

- Platform-specific quirks (Slack thread semantics, Discord rate limits) leak into
  adapter logic — keep adapter-specific docs under `docs/design-docs/channel-<name>.md`.
- Inbound flood from a noisy room saturates the bounded queue — observe via the
  `channel.delivery_error` hook; users can configure a per-conversation rate limit.

## Migration Notes For The QQ Port

- Move `legacy/src/channel/qq/qq-api-client.cpp` to `src/oran-channel-qq/api-client.cpp`,
  but rewrite its libcurl direct use to go through `oran-http::Client`.
- OAuth + token refresh: own state inside `oran-channel-qq::TokenStore`, not in a
  global.
- Attachment cache: lift into `oran-channel::AttachmentCache` (shared by all adapters).
- Group vs. direct-message: keep the legacy distinction; the capability matrix
  expresses it.

## Validation

```sh
xmake f --channel_qq=y --channel_webhook=y --channel_discord=n
xmake build oran-channel oran-channel-qq oran-channel-webhook
xmake test test-channel test-channel-qq
scripts/bench-compare.sh channel
```
