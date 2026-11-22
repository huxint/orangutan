# bench-config

`bench-config` measures the `oran-config` loading and typed-section
parse surface.

## Scenarios

### `bench-config` block

- `config.parse_memory`: parses an in-memory JSON document through
  `Config::parse`. The fixture includes `runtime.prompt.active_tools`
  so prompt config remains covered by the startup loading path.
- `config.load_file_example`: reads and parses the checked-in
  `config.example.json` through `Config::load_file`.

### `bench-config/permissions` block

Compare permission blocks with different rule counts and input patterns:

- `config.parse_permissions_empty`: parses a config that carries an
  empty `permissions: {}` block. Documents the lift of having the
  typed surface plumbed without any rules to resolve.
- `config.parse_permissions_typed`: parses a config with 16 mixed-
  scope permission rules and one `agents.researcher.permissions`
  overlay. Bootstrap compiles the parsed rules at session construction.
- `config.parse_permissions_with_input_patterns`: parses a 14-rule
  block where four `deny` rules carry an `input_pattern` re2 source
  pattern. Documents the per-pattern re2 compile cost the load-time
  validator pays. Bootstrap separately compiles the retained source into runtime
  patterns; that conversion is covered by `bench-bootstrap`.
