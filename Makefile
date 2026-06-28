GENERATED := griffin.generated.h griffin.generated.inc griffin.generated.ld griffin.generated.vh griffin.generated.refs.h

codegen: $(GENERATED)

$(GENERATED): .codegen.stamp

.codegen.stamp: griffin.yml codegen.py
	python3 codegen.py griffin.yml
	touch $@

validate:
	python3 -m check_jsonschema --schemafile hw_schema.yml griffin.yml

.PHONY: codegen validate check-generated check-codegen install-hooks

# Verify the committed generated files match what codegen would produce now,
# without touching the working tree (regenerate to a temp dir and diff).
# Used by the pre-commit hook; subdir builds regenerate automatically instead.
check-generated:
	@tmp=$$(mktemp -d); \
	python3 codegen.py griffin.yml --outdir "$$tmp" >/dev/null; \
	rc=0; for f in $(GENERATED); do \
	  diff -q "$$tmp/$$f" "$$f" >/dev/null 2>&1 || { \
	    echo "STALE: $$f  (run 'make codegen' and stage it)"; rc=1; }; \
	done; rm -rf "$$tmp"; exit $$rc

# Commit-time guard for gitignored generated files: the committed griffin.yml
# must regenerate cleanly so a fresh checkout can build.  Schema validation is
# run as a non-blocking advisory (run 'make validate' to see/fix drift).
check-codegen:
	@tmp=$$(mktemp -d); \
	if python3 codegen.py griffin.yml --outdir "$$tmp" >/dev/null 2>&1; then \
	  echo "codegen OK"; rc=0; \
	else echo "ERROR: codegen failed on griffin.yml"; rc=1; fi; \
	rm -rf "$$tmp"; \
	python3 -m check_jsonschema --schemafile hw_schema.yml griffin.yml >/dev/null 2>&1 \
	  || echo "advisory: 'make validate' reports schema drift (not blocking this commit)"; \
	exit $$rc

# One-time: point git at the repo-tracked hooks/ directory.
install-hooks:
	git config core.hooksPath hooks
	@echo "core.hooksPath set to ./hooks"
