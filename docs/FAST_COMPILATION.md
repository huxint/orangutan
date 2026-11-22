# Compile Cost

Keep heavy dependencies in implementation files and use narrow public headers.
Prefer value types and ordinary functions over template machinery. Split a
translation unit by responsibility when measurement identifies a hotspot.

`xmake/targets.lua` owns library boundaries; each library builds separately with
the stable PCH. GUI, messaging and scheduling dependencies are absent from the
core build. The existing vector backend remains optional.

Run affected builds while iterating. Use `scripts/measure-tu.sh` or
`scripts/check-compile-budget.sh` when investigating a build slowdown or explicitly
optimizing compilation. Compare the same compiler, hardware, mode and job count.
Routine changes do not require before/after measurements. Thresholds and reference
hardware live in
[compile-budget](rules/compile-budget.md).

Hosted functional C++ jobs are separate from reference-hardware performance
provisioning. Report unmeasured cost as unverified.
