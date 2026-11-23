.PHONY: check-docs check-repo ci help

help:
	@echo "Available targets:"
	@echo "  make ci          check docs, dependencies, headers, hygiene and shell syntax"
	@echo "  make check-docs  verify required docs exist"
	@echo "  make check-repo  check docs and repository hygiene"

check-docs:
	./scripts/check-docs.sh

check-repo:
	./scripts/check-docs.sh
	./scripts/check-repo-hygiene.sh

ci:
	./scripts/ci.sh
