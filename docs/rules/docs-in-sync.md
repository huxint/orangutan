# Current Contracts Stay In Sync

Update the owning document when a change affects public behavior, interfaces,
configuration, build commands, dependencies or architectural invariants. Internal
refactors and tests do not require unrelated documentation churn.

| Change | Owner |
| --- | --- |
| Library, executable or dependency direction | Architecture and relevant design contract. |
| Public behavior or effect boundary | Relevant design contract. |
| Configuration or usage | Example configuration, state contract and README. |
| Build, toolchain or CI | BUILD_SYSTEM, CICD and relevant rule. |
| Work spanning commits or unresolved defect | Active plan or live debt. |

One current contract owns each fact. Keep indexes short. Delete replaced feature
specifications, completed plans and review narratives after their lasting
constraints have been absorbed. Git owns history.

`make ci` checks required documents, indexes, referenced scripts/headers, build
inventory, package versions, test/bench parity, configuration shape and the hook
and capability catalogues. It also checks dependency direction and public include
hygiene. Review checks current behavior and link validity;
structural checks cannot infer every semantic contract.
