# Module Boundaries

[Architecture](../ARCHITECTURE.md) owns the library inventory and graph.
`scripts/check-deps.sh` enforces dependency direction from composition toward
values and platform primitives.

A public header exposes values, explicit ownership and narrow service contracts.
Keep parser, SQLite, curl and other heavy implementation types in .cpp files or
private headers. Prefer a free function for a pure transformation; use an owning
type when a resource or cache has a lifetime to manage.

Split a translation unit when responsibilities or measured compile cost require
it. A new library needs an actual boundary, its callers and test/bench buckets.
Avoid public forwarding layers that add no domain contract.

Cache policy belongs to a concrete resource owner, such as storage's
per-connection statement cache. Shared convenience templates need runtime
consumers before becoming core APIs.
[Error handling](../rules/error-handling.md) owns the result boundary.
