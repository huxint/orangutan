SLUG ?=

.PHONY: check-docs check-repo ci new-plan help

help:
	@echo "Available targets:"
	@echo "  make ci                        check docs, dependencies, headers, hygiene and shell syntax"
	@echo "  make check-docs                verify required docs exist"
	@echo "  make check-repo                full repo hygiene check"
	@echo "  make new-plan SLUG=...         scaffold an execution plan"

check-docs:
	./scripts/check-docs.sh

check-repo:
	./scripts/check-docs.sh
	./scripts/check-repo-hygiene.sh

ci:
	./scripts/ci.sh

new-plan:
	@if [ -z "$(SLUG)" ]; then echo "usage: make new-plan SLUG=my-plan"; exit 1; fi
	./scripts/new-exec-plan.sh "$(SLUG)"
