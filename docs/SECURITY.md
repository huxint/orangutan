# Runtime Security

The tool execution boundary validates input, resolves workspace authority,
evaluates permissions and records the decision before an effect. Denial and
unanswered approval requests do not run a handler. Input rewritten by hooks
is checked under the same policy before execution.

Filesystem effects use pinned directory/file handles. Path display strings are
not authority. Read/write root grants are explicit and mutation guards detect
replaced targets. Memory operations bind their scope from the owning session.

Provider secrets are resolved from configured references after route validation.
They stay out of errors, audit summaries and cached prompts. The selected state
directory is private; backup and import operations must preserve that protection.

Child-agent authority attenuation is planned before collaboration. A model's
request or an advertised capability cannot grant authority.

[Permissions](design-docs/permissions-and-hooks.md),
[file authority](design-docs/io-runtime.md) and
[supply chain](SUPPLY_CHAIN_SECURITY.md) own the detailed boundaries.
