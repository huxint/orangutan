# Tech Debt Tracker

Track known debt that is real enough to preserve but not urgent enough to block the
current task.

Order rows by *date discovered* (newest at the top). Add a row in the same PR that
created the debt.

| Date discovered | Area | Debt | Why It Exists | Planned Follow-Up |
| --------------- | ---- | ---- | ------------- | ------------------ |
| 2026-05-14 | docs | Build skeleton scripts (`scripts/check-deps.sh`, `scripts/check-includes.sh`, `scripts/measure-tu.sh`, `scripts/check-compile-budget.sh`) referenced from rules but not yet implemented. | Framework lands before code; scripts will be authored alongside the first C++ slice. | Implement when MVP loop lands (spec 0001). |
| 2026-05-14 | docs | `config.example.json` for v2 not yet generated. | Schema not finalized until first config bootstrap PR. | Generate from `oran-config` C++ types via build step. |
| 2026-05-14 | bench | A-vs-B scenarios listed in `bench/<lib>/README.md` are placeholders. | Bench code follows each library's MVP. | Land with the corresponding library's first PR. |
| 2026-05-14 | docs/web | Frontend stack choice (Preact vs. plain JS) not yet decided. | Defer until the first useful UI flow. | Decide in `docs/exec-plans/active/<date>-web-mvp.md`. |
