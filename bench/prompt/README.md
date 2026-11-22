# bench-prompt

`bench-prompt` measures pure prompt rendering and native tool fingerprinting.
`prompt.render_native_catalog` uses seven built-in names with no-input fixture
schemas;
`prompt.render_native_subset` uses two definitions from the same fixture.
Neither path constructs an executor, discovers tools or renders schema text.

`prompt.rebuild_prefix_each_iteration` and `prompt.reuse_prefix_per_turn` compare
eight prefix observations with repeated rendering or a single owned render.
Both use the same seven definitions, 4 KiB preamble and 8 KiB memory text.
These measure prefix preparation only; provider request copies, conversation,
tool execution and network latency are outside the fixture.

Prompt tests cover text joining, content-derived cache identities and native-definition
invalidation. Agent tests cover prefix reuse and refresh across turns while
typed conversation advances. Tool selection has its own
benchmarks in [bench-tool](../tool/README.md).
