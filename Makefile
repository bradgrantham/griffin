GENERATED := griffin.generated.h griffin.generated.inc griffin.generated.ld griffin.generated.vh griffin.generated.refs.h

codegen: $(GENERATED)

$(GENERATED): .codegen.stamp

.codegen.stamp: griffin.yml codegen.py
	python3 codegen.py griffin.yml
	touch $@

validate:
	python3 -m check_jsonschema --schemafile hw_schema.yml griffin.yml

.PHONY: codegen validate check-generated install-hooks

# Hard gate (schema valid + generated files current).  Runs 'validate' first,
# then regenerates to a temp dir and diffs against the tracked copies — no
# working-tree mutation.  Used by the pre-commit hook; subdir builds regenerate
# automatically instead.
check-generated: validate
	@tmp=$$(mktemp -d); \
	python3 codegen.py griffin.yml --outdir "$$tmp" >/dev/null; \
	rc=0; for f in $(GENERATED); do \
	  diff -q "$$tmp/$$f" "$$f" >/dev/null 2>&1 || { \
	    echo "STALE: $$f  (run 'make codegen' and stage it)"; rc=1; }; \
	done; rm -rf "$$tmp"; exit $$rc

# One-time: point git at the repo-tracked hooks/ directory.
install-hooks:
	git config core.hooksPath hooks
	@echo "core.hooksPath set to ./hooks"
