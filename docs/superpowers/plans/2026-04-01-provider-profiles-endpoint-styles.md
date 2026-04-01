# Provider Profiles And Endpoint Styles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace per-agent provider credentials with shared profiles plus per-model endpoint styles, then add first-class support for `openai-responses`, `openai-chat-completions`, and `anthropic-messages`.

**Architecture:** Move connection identity into top-level `profiles`, move per-model protocol/default metadata into `profiles.<name>.models`, and make runtime assembly resolve each agent from `profile + model`. Provider execution then branches on `endpoint_style` instead of the old provider string, while preserving the existing agent loop and tool-calling model.

**Tech Stack:** C++23, `nlohmann::json`, `cpp-httplib`, Catch2, Xmake

---

## File Structure

**Config schema and parsing**
- Modify: `src/config/config.hpp`
- Modify: `src/config/config.cpp`
- Modify: `src/config/config-detail.hpp`
- Modify: `src/config/config-sections-core.cpp`
- Modify: `src/config/config-sections-integrations.cpp`
- Modify: `src/config/secret-fields.hpp`
- Modify: `src/config/secret-fields.cpp`
- Modify: `src/config/secret-protection-file.cpp`
- Modify: `tests/config/config-test.cpp`
- Modify: `tests/config/config-save-test.cpp`
- Modify: `tests/config/config-secret-test.cpp`
- Modify: `config.example.json`
- Modify: `web/src/components/admin/AgentsPage.tsx`

**Runtime resolution**
- Modify: `src/bootstrap/config-builder.cpp`
- Modify: `src/bootstrap/config-builder.hpp`
- Modify: `src/bootstrap/config-bootstrap.cpp`
- Modify: `src/bootstrap/agent-runtime.hpp`
- Modify: `src/bootstrap/agent-runtime.cpp`
- Modify: `src/bootstrap/channel-serve.hpp`
- Modify: `src/bootstrap/channel-serve.cpp`
- Modify: `src/bootstrap/bootstrap.cpp`
- Modify: `src/web/web-routes.cpp`
- Modify: `src/subagent/subagent-manager.hpp`
- Modify: `src/subagent/subagent-manager.cpp`
- Modify: `tests/bootstrap/bootstrap-test.cpp`
- Modify: `tests/bootstrap/channel-serve-test.cpp`
- Modify: `tests/bootstrap/runtime-agent-runtime-test.cpp`
- Modify: `tests/subagent/subagent-manager-test.cpp`
- Modify: `tests/integration/subagent-integration-test.cpp`
- Modify: `tests/web/web-routes-test.cpp`
- Modify: `tests/web/web-chat-test.cpp`

**Provider protocol selection**
- Modify: `src/providers/provider.hpp`
- Modify: `src/providers/provider-factory.cpp`
- Modify: `src/providers/openai-provider.hpp`
- Modify: `src/providers/openai-provider.cpp`
- Modify: `src/providers/anthropic-provider.hpp`
- Modify: `src/providers/anthropic-provider.cpp`
- Test: `tests/providers/provider-fallback-test.cpp`
- Create or modify targeted provider tests under `tests/providers/`

**Skill / config surface cleanup**
- Modify: `src/bootstrap/cli-options.cpp`
- Modify: `src/web/web-routes.cpp`
- Verify: `src/skills/skill-loader.cpp`
- Verify: `tests/skills/skill-loader-test.cpp`
- Verify: `xmake/packages.lua`
- Verify: `xmake/targets.lua`

## Task 1: Replace the Config Schema With Profiles And Model Catalogs

**Files:**
- Modify: `src/config/config.hpp`
- Modify: `src/config/config.cpp`
- Modify: `src/config/config-detail.hpp`
- Modify: `src/config/config-sections-core.cpp`
- Modify: `src/config/config-sections-integrations.cpp`
- Modify: `tests/config/config-test.cpp`
- Modify: `tests/config/config-save-test.cpp`
- Modify: `config.example.json`

- [ ] **Step 1: Write failing config tests for the new schema**

Add coverage for:
- `profiles.<name>.base_url`
- `profiles.<name>.api_key`
- `profiles.<name>.headers`
- `profiles.<name>.models.<model>.endpoint_style`
- `profiles.<name>.models.<model>.context_window`
- `profiles.<name>.models.<model>.cost`
- `agents.<name>.profile`
- default-agent behavior when `agents.default` is omitted but other profile-backed agents exist
- rejection of missing profile/model references during runtime-building tests later

Example fixture:

```json
{
  "profiles": {
    "A": {
      "base_url": "https://gateway.example.com",
      "api_key": "${API_KEY}",
      "headers": {"X-App-Id": "orangutan"},
      "models": {
        "gpt-4.1": {
          "endpoint_style": "openai-responses",
          "max_tokens": 32000,
          "thinking": "medium"
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

- [ ] **Step 2: Run config tests to verify they fail**

Run: `xmake build -j 1 test-config && xmake run test-config`  
Expected: failures because `profiles`, model metadata, and `agents.*.profile` are not recognized yet.

- [ ] **Step 3: Implement the new config data model and JSON parsing**

Add focused structs in `src/config/config.hpp`, for example:

```cpp
struct ModelConfig {
    std::string endpoint_style;
    std::optional<int> max_tokens;
    std::optional<int> context_window;
    std::string thinking = "none";
    std::optional<ModelCostConfig> cost;
};

struct ProfileConfig {
    std::string base_url;
    std::string api_key;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, ModelConfig> models;
};
```

Then:
- remove legacy `provider` / `base_url` / `api_key` fields from `Config` and `AgentConfig`
- add `profile` to agents
- parse/save `profiles`
- parse/save `context_window` and `cost` metadata without dropping them on round-trip
- keep `config.json` round-trippable

- [ ] **Step 4: Update shipped examples and admin UI copy**

Update `config.example.json` to use `profiles` and `agents.*.profile`.  
Update `web/src/components/admin/AgentsPage.tsx` to stop suggesting obsolete per-agent provider settings.

- [ ] **Step 5: Re-run config tests**

Run: `xmake build -j 1 test-config && xmake run test-config`  
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/config/config.hpp src/config/config.cpp src/config/config-detail.hpp src/config/config-sections-core.cpp src/config/config-sections-integrations.cpp tests/config/config-test.cpp tests/config/config-save-test.cpp config.example.json web/src/components/admin/AgentsPage.tsx
git commit -m "refactor(config): add profiles and model endpoint catalogs"
```

## Task 2: Rework Secret Protection For Profile-Level Credentials

**Files:**
- Modify: `src/config/secret-fields.hpp`
- Modify: `src/config/secret-fields.cpp`
- Modify: `src/config/secret-protection-file.cpp`
- Modify: `tests/config/config-secret-test.cpp`

- [ ] **Step 1: Write failing tests for profile credential protection**

Cover:
- `profiles.<name>.api_key` gets encrypted
- nested model metadata does not get treated as secrets
- backup and permissions behavior stays intact

- [ ] **Step 2: Run the config secret tests to verify failure**

Run: `xmake build -j 1 test-config && xmake run test-config`  
Expected: failures because the secret protector still targets legacy agent/provider fields.

- [ ] **Step 3: Implement profile-level secret selection**

Update JSON-tree secret protection to target profile credentials only, for example:

```cpp
if (auto it = root.find("profiles"); it != root.end() && it->is_object()) {
    for (auto profile_it = it->begin(); profile_it != it->end(); ++profile_it) {
        protected_count += protect_secret_value(profile_it.value(), "api_key", "profiles.api_key", password);
    }
}
```

Also update the secret field specs so runtime decryption resolves profile API keys from the new schema.

- [ ] **Step 4: Re-run config tests**

Run: `xmake build -j 1 test-config && xmake run test-config`  
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/config/secret-fields.hpp src/config/secret-fields.cpp src/config/secret-protection-file.cpp tests/config/config-secret-test.cpp
git commit -m "refactor(config): protect profile api keys in json config"
```

## Task 3: Resolve Agent Runtimes From Profile Plus Model

**Files:**
- Modify: `src/bootstrap/config-builder.cpp`
- Modify: `src/bootstrap/config-builder.hpp`
- Modify: `src/bootstrap/config-bootstrap.cpp`
- Modify: `src/bootstrap/agent-runtime.hpp`
- Modify: `src/bootstrap/agent-runtime.cpp`
- Modify: `src/bootstrap/channel-serve.hpp`
- Modify: `src/bootstrap/channel-serve.cpp`
- Modify: `src/bootstrap/bootstrap.cpp`
- Modify: `src/subagent/subagent-manager.hpp`
- Modify: `src/subagent/subagent-manager.cpp`
- Modify: `tests/bootstrap/bootstrap-test.cpp`
- Modify: `tests/bootstrap/channel-serve-test.cpp`
- Modify: `tests/bootstrap/runtime-agent-runtime-test.cpp`
- Modify: `tests/subagent/subagent-manager-test.cpp`
- Modify: `tests/integration/subagent-integration-test.cpp`

- [ ] **Step 1: Write failing runtime-resolution tests**

Add tests for:
- resolving an agent from `profile + model`
- failure on unknown profile
- failure on unknown model inside a known profile
- propagation of profile headers and resolved endpoint style into runtime config
- CLI-selected agent resolution through `resolve_selected_agent()` / startup config bootstrap paths
- subagent child runtime resolution after `SubagentChildRuntimeConfig` drops the old `provider_name` field

- [ ] **Step 2: Run bootstrap tests to verify failure**

Run: `xmake build -j 1 test-bootstrap && xmake run test-bootstrap`  
Expected: failures because runtime structs still expect `provider_name`, `base_url`, and `api_key` directly on each agent.

- [ ] **Step 3: Implement runtime resolution structs**

Replace legacy provider fields in runtime config with resolved endpoint fields:

```cpp
struct ResolvedModelEndpoint {
    std::string profile_name;
    std::string endpoint_style;
    std::string base_url;
    std::string api_key;
    std::unordered_map<std::string, std::string> headers;
    std::string model;
    std::optional<int> default_max_tokens;
    std::string thinking;
};
```

Then make `build_agent_runtime_configs()`:
- look up the agent profile
- look up the chosen model inside that profile
- merge profile connectivity plus model metadata
- fail fast with actionable errors

Also update `src/bootstrap/config-bootstrap.cpp` so CLI overrides only touch fields that still exist after the schema change. The old `selected_agent.provider` / `selected_agent.base_url` writes must be removed or remapped to the new profile-based runtime selection flow.

Update `src/bootstrap/agent-runtime.cpp`, `src/bootstrap/channel-serve.cpp`, and `src/web/web-routes.cpp` in the same task so runtime assembly consumes the resolved endpoint config directly. All three files currently sit on the compile path that still expects `provider_name` and direct base URL fields.

- [ ] **Step 4: Propagate resolved endpoint config to child/subagent runtimes**

Update `SubagentChildRuntimeConfig`, `AgentRuntimeBuildInput`, and `AgentRuntimeConfig` so child workers inherit resolved endpoint settings instead of the removed provider string.

- [ ] **Step 5: Re-run bootstrap tests**

Run: `xmake build -j 1 test-bootstrap && xmake run test-bootstrap`  
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/bootstrap/config-builder.cpp src/bootstrap/config-builder.hpp src/bootstrap/config-bootstrap.cpp src/bootstrap/agent-runtime.hpp src/bootstrap/agent-runtime.cpp src/bootstrap/channel-serve.hpp src/bootstrap/channel-serve.cpp src/bootstrap/bootstrap.cpp src/web/web-routes.cpp src/subagent/subagent-manager.hpp src/subagent/subagent-manager.cpp tests/bootstrap/bootstrap-test.cpp tests/bootstrap/channel-serve-test.cpp tests/bootstrap/runtime-agent-runtime-test.cpp tests/subagent/subagent-manager-test.cpp tests/integration/subagent-integration-test.cpp
git commit -m "refactor(runtime): resolve agents through profiles and model catalogs"
```

## Task 4: Replace Provider Names With Endpoint Styles

**Files:**
- Modify: `src/providers/provider.hpp`
- Modify: `src/providers/provider-factory.cpp`
- Modify: `tests/providers/provider-fallback-test.cpp`

- [ ] **Step 1: Write failing provider-factory tests for endpoint styles**

Cover:
- `openai-responses`
- `openai-chat-completions`
- `anthropic-messages`
- rejection of unknown endpoint styles

- [ ] **Step 2: Run provider tests to verify failure**

Run: `xmake build -j 1 test-providers && xmake run test-providers`  
Expected: failures because provider creation still switches on `provider_name`.

- [ ] **Step 3: Refactor `ProviderEndpoint` and the factory API**

Change the endpoint descriptor to something like:

```cpp
struct ProviderEndpoint {
    std::string endpoint_style;
    std::string api_key;
    std::string model;
    std::string base_url;
    std::unordered_map<std::string, std::string> headers;
    std::optional<int> default_max_tokens;
    std::string thinking;
};
```

Update factory entry points and fallback chain construction to carry `endpoint_style` instead of `provider_name`.

- [ ] **Step 4: Re-run provider tests**

Run: `xmake build -j 1 test-providers && xmake run test-providers`  
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/providers/provider.hpp src/providers/provider-factory.cpp tests/providers/provider-fallback-test.cpp
git commit -m "refactor(providers): select backends by endpoint style"
```

## Task 5: Implement OpenAI Responses, OpenAI Chat Completions, and Anthropic Messages

**Files:**
- Modify: `src/providers/openai-provider.hpp`
- Modify: `src/providers/openai-provider.cpp`
- Modify: `src/providers/anthropic-provider.hpp`
- Modify: `src/providers/anthropic-provider.cpp`
- Create or modify: `tests/providers/openai-provider-test.cpp`
- Create or modify: `tests/providers/anthropic-provider-test.cpp`

- [ ] **Step 1: Write failing provider behavior tests**

Add focused tests for:
- request path selection:
  - `/v1/responses`
  - `/v1/chat/completions`
  - `/v1/messages`
- streaming event parsing for each style
- tool call accumulation for both OpenAI styles
- profile header forwarding
- reasoning/thinking parameter mapping where supported

- [ ] **Step 2: Run provider tests to verify failure**

Run: `xmake build -j 1 test-providers && xmake run test-providers`  
Expected: failures because the current OpenAI provider only talks to Chat Completions and Anthropic is only implicit.

- [ ] **Step 3: Split protocol-specific request/response handling**

Implement a protocol branch keyed by endpoint style:

```cpp
if (endpoint.endpoint_style == "openai-responses") {
    url = base_url_ + "/v1/responses";
} else if (endpoint.endpoint_style == "openai-chat-completions") {
    url = base_url_ + "/v1/chat/completions";
} else if (endpoint.endpoint_style == "anthropic-messages") {
    url = base_url_ + "/v1/messages";
}
```

Apply shared headers and map model metadata:
- use model-level `max_tokens` as the default request limit
- map `thinking` enum only when the target protocol supports it
- ignore unsupported options cleanly instead of inventing fake fields

- [ ] **Step 4: Re-run provider tests**

Run: `xmake build -j 1 test-providers && xmake run test-providers`  
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/providers/openai-provider.hpp src/providers/openai-provider.cpp src/providers/anthropic-provider.hpp src/providers/anthropic-provider.cpp tests/providers/
git commit -m "feat(providers): support responses chat-completions and anthropic-messages styles"
```

## Task 6: Revalidate Tooling, Web, and Config Editing Surfaces

**Files:**
- Modify: `tests/tools/registry/tool-registry-test.cpp`
- Modify: `tests/web/web-routes-test.cpp`
- Modify: `tests/web/web-chat-test.cpp`
- Modify: `src/web/admin-routes.cpp`
- Modify: `src/web/web-routes.cpp`
- Modify: `src/bootstrap/cli-options.cpp`

- [ ] **Step 1: Add failing tests for config API round-trips with profiles**

Cover:
- admin config endpoints returning/saving the new profile schema
- config file access tests still targeting `config.json`
- web-side agent listings reflecting `profile + model` instead of removed provider names

- [ ] **Step 2: Run affected targets to verify failure**

Run:
- `xmake build -j 1 test-tools && xmake run test-tools`
- `xmake build -j 1 test-web`

Expected: failures anywhere that still assumes per-agent provider fields.

- [ ] **Step 3: Implement the minimal surface fixes**

Update admin/web serialization and any remaining config editing helpers so they preserve:
- `profiles`
- `agents.*.profile`
- model metadata fields without dropping unknown-but-supported keys

In the same step, remove obsolete CLI flags that no longer make sense without compatibility:
- remove or reject `--provider`
- remove or reject `--base-url`

The plan should treat this as a schema cleanup, not a remap, because the change explicitly does not preserve the old configuration contract.

- [ ] **Step 4: Re-run affected targets**

Run:
- `xmake build -j 1 test-tools && xmake run test-tools`
- `xmake build -j 1 test-web`
- `xmake run test-web`  
If sandbox binding blocks localhost, re-run `xmake run test-web` outside the sandbox.

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/tools/registry/tool-registry-test.cpp tests/web/web-routes-test.cpp tests/web/web-chat-test.cpp src/web/admin-routes.cpp src/web/web-routes.cpp src/bootstrap/cli-options.cpp
git commit -m "test(web): cover profile-based model endpoint config"
```

## Final Verification

- [ ] Run:
  - `xmake build -j 1 orangutan-lib`
  - `xmake build -j 1 test-config`
  - `xmake run test-config`
  - `xmake build -j 1 test-providers`
  - `xmake run test-providers`
  - `xmake build -j 1 test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build -j 1 test-subagent`
  - `xmake run test-subagent`
  - `xmake build -j 1 test-integration`
  - `xmake run test-integration`
  - `xmake build -j 1 test-tools`
  - `xmake run test-tools`
  - `xmake build -j 1 test-web`
  - `xmake run test-web`

- [ ] Confirm `rg -n "provider_name|provider\\\"|api_key|base_url" src tests config.example.json` only reports legitimate profile/model fields and no obsolete agent-level schema assumptions.

- [ ] Commit final cleanup:

```bash
git add -A
git commit -m "chore: finalize profile-based endpoint style migration"
```
