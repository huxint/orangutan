# Config Secret Protection

`orangutan` can keep supported config secrets encrypted at rest inside `~/.orangutan/config.toml`.

Supported fields:
- `[agent].api_key`
- `[agents.<name>].api_key`
- `[qq].client_secret`
- `[[qq_bots]].client_secret`

## Protect Existing Secrets

Run:

```bash
orangutan --protect-config-secrets
```

Optional forms:
- `orangutan --protect-config-secrets /path/to/config.toml`
- `orangutan --config-password "<password>" --protect-config-secrets`

If `--config-password` is omitted, `orangutan` checks `ORANGUTAN_CONFIG_PASSWORD` and then prompts on an interactive terminal. The protect flow writes a backup next to the config file as `config.toml.bak`.

## Unlock Protected Secrets

Protected values use the `enc:v1:...` string format. At startup, `orangutan` resolves the password in this order:
1. `--config-password`
2. `ORANGUTAN_CONFIG_PASSWORD`
3. Interactive terminal prompt

If a protected secret is present in a headless run and no password source is available, startup fails before any provider or channel work begins.

## Recommended Practice

Environment variables remain supported and are still the simplest option for automation. Use protected config secrets when you want a single local config file without leaving provider API keys or QQ client secrets readable at rest.
