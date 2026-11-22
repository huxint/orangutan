# Execution Plans

Use a plan for a large change, a new architectural boundary or work spanning
multiple commits. Put active plans under `docs/exec-plans/active/` with a date and
domain name. `make new-plan SLUG=...` creates the template.

State the objective, current contracts, smallest complete slice, verification
and completion criteria. Keep future work separate from implemented behavior.
Update progress as evidence arrives; avoid a narrative changelog.

When complete, put durable invariants in their owning design/rule document and
remaining defects in live debt, then delete the plan. Git preserves history.
