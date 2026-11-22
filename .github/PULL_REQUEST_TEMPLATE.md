## Change

Describe the concrete problem and resulting behavior. Link the owning contract
and execution plan when applicable.

## Validation

- [ ] Affected build and tests pass.
- [ ] `make ci` passes.
- [ ] Runtime changes pass the full release suite.
- [ ] Lifetime changes pass ASan/UBSan checks.
- [ ] New or rewritten tests detect their intended failure.

## Contracts

- [ ] Changed API, configuration, build and behavior contracts are updated.
- [ ] Effects use the permission, hook and cancellation boundaries.
- [ ] Borrowed services outlive their asynchronous work.
- [ ] Dependencies and public headers follow the repository rules.
- [ ] Replaced code and stale documents are removed; user data is preserved.

Remaining risks or follow-up debt:
