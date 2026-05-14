PROJECT ?=
SLUG    ?=
LIB     ?=

.PHONY: init check-docs check-repo ci new-history new-plan bench-compare release-package help

help:
	@echo "Available targets:"
	@echo "  make init PROJECT=...          rename template placeholders to the new project"
	@echo "  make ci                        run repo-wide checks (docs + hygiene + shell-lint)"
	@echo "  make check-docs                verify required docs exist"
	@echo "  make check-repo                full repo hygiene check"
	@echo "  make new-history SLUG=...      scaffold a history entry"
	@echo "  make new-plan SLUG=...         scaffold an execution plan"
	@echo "  make bench-compare LIB=...     run a library's benchmarks and compare to baseline"
	@echo "  make release-package           run the release packaging script (stub until v1)"

init:
	@if [ -z "$(PROJECT)" ]; then echo "usage: make init PROJECT=my-project"; exit 1; fi
	./scripts/init-project.sh "$(PROJECT)"

check-docs:
	./scripts/check-docs.sh

check-repo:
	./scripts/check-docs.sh
	./scripts/check-repo-hygiene.sh

ci:
	./scripts/ci.sh

new-history:
	@if [ -z "$(SLUG)" ]; then echo "usage: make new-history SLUG=my-change"; exit 1; fi
	./scripts/new-history.sh "$(SLUG)"

new-plan:
	@if [ -z "$(SLUG)" ]; then echo "usage: make new-plan SLUG=my-plan"; exit 1; fi
	./scripts/new-exec-plan.sh "$(SLUG)"

bench-compare:
	@if [ -z "$(LIB)" ]; then echo "usage: make bench-compare LIB=memory"; exit 1; fi
	./scripts/bench-compare.sh "$(LIB)"

release-package:
	./scripts/release-package.sh
