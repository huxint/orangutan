# Modules And PCH

Public headers expose narrow value and ownership contracts. Heavy third-party
headers stay private. Consumers may include a narrow header or library umbrella.
`src/oran-<lib>/_impl/` holds implementation details shared by that library's TUs.

The stable PCH in `include/oran/_pch.hpp` contains low-cost standard headers and
core value types. Frequently edited runtime headers and generated dependencies
do not belong in it. A per-target exception needs measured compile-cost evidence.

The supported build uses ordinary headers and static libraries. A future module
migration needs a complete build/test and measured cost comparison before adding
a configuration option. C++26 and the [compile budget](compile-budget.md) remain
binding.
