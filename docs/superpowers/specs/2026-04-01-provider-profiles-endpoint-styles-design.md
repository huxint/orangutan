# Provider Profiles And Endpoint Styles Design

**Date:** 2026-04-01

## Goal

Replace the current agent-level `provider` / `base_url` / `api_key` configuration with a profile-based model catalog. Agents should reference a shared `profile` plus a model name, while each profile declares its reachable endpoint, shared authentication, shared headers, and per-model endpoint style plus model metadata.

## Approved Configuration Shape

The new top-level config will use `profiles` instead of provider strings embedded in each agent:

```json
{
  "profiles": {
    "A": {
      "base_url": "https://gateway.example.com",
      "api_key": "${GATEWAY_API_KEY}",
      "headers": {
        "X-App-Id": "orangutan"
      },
      "models": {
        "gpt-4.1": {
          "endpoint_style": "openai-responses",
          "max_tokens": 32000,
          "context_window": 128000,
          "thinking": "medium",
          "cost": {
            "input": 2.0,
            "output": 8.0
          }
        },
        "claude-sonnet-4-20250514": {
          "endpoint_style": "anthropic-messages",
          "max_tokens": 64000,
          "context_window": 200000,
          "thinking": "high"
        }
      }
    }
  },
  "agents": {
    "default": {
      "profile": "A",
      "model": "gpt-4.1"
    }
  }
}
```

## Rules

- The old agent-level `provider`, `base_url`, and `api_key` fields will be removed.
- Compatibility with the removed schema is intentionally not required.
- Agents resolve runtime connectivity through `profile` plus `model`.
- `agents.default` is no longer synthesized. If a code path needs the `default` agent, it must exist explicitly in config; otherwise startup should fail with a clear error.
- Profile-level `base_url`, `api_key`, and `headers` are shared by all models under that profile.
- Model-level configuration may declare endpoint style and model metadata/defaults, but must not override profile-level connection identity such as `base_url` or `api_key`.

## Supported Endpoint Styles

The runtime will support exactly these protocol styles:

- `openai-responses`
- `openai-chat-completions`
- `anthropic-messages`

The existing OpenAI-compatible implementation will be split so it can talk to both Chat Completions and Responses. Anthropic will be represented explicitly as `anthropic-messages`.

## Runtime Behavior

- Runtime construction resolves an agent to its selected profile and model definition.
- Credential lookup order is: explicit CLI API key override, then `profiles.<name>.api_key`, then generic `LLM_API_KEY` if present. Old provider-specific environment fallbacks are removed with the old provider schema.
- Resolved runtime state must keep enough data to survive bootstrap, web, and subagent handoff without re-deriving provider strings. At minimum the resolved endpoint shape carried through runtime layers includes `profile_name`, `endpoint_style`, `base_url`, `api_key`, shared `headers`, selected `model`, and model defaults such as `max_tokens` and `thinking`.
- `endpoint_style` selects the request path, payload shape, streaming parser, and tool-call decoding logic.
- Shared request headers from the profile are attached to every request.
- Model defaults are applied where the selected endpoint style supports them.
  - `max_tokens` should be used as the runtime default request limit.
  - `thinking` is an enum with values `none`, `low`, `medium`, `high`, `max` and should be mapped only for endpoint styles that support reasoning controls.
  - `context_window` and `cost` are metadata for now; they should be parsed and preserved but do not require budgeting or auto-selection logic in this change.
- Fallback model names are resolved inside the same profile as the primary model. Each fallback model carries its own `endpoint_style`, defaults, and metadata when building the provider chain.
- User-facing CLI help text, startup errors, and admin config editing surfaces must be updated to remove references to legacy per-agent `provider`, `base_url`, and provider-specific environment variables.

## Non-Goals

- No compatibility shim for the removed config schema.
- No automatic migration of old config files.
- No pricing engine, budgeting workflow, or context-window enforcement logic.
- No expansion beyond the three approved endpoint styles in this change.

## Validation

The change is complete when:

- JSON config parsing/saving round-trips the new profile schema.
- Agent runtime creation resolves `profile` plus `model` and fails clearly on missing references.
- OpenAI Chat Completions, OpenAI Responses, and Anthropic Messages all have targeted provider tests.
- Affected bootstrap, tool, and web tests pass against the new schema.
- Admin config APIs and the React config pages preserve `profiles`, `agents.*.profile`, and model defaults without stripping them on save.
