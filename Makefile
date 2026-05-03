GENERATED := griffin.generated.h griffin.generated.inc griffin.generated.ld griffin.generated.vh griffin.generated.refs.h

codegen: $(GENERATED)

$(GENERATED): .codegen.stamp

.codegen.stamp: griffin.yml codegen.py
	python3 codegen.py griffin.yml
	touch $@

validate:
	python3 -m check_jsonschema --schemafile hw_schema.yml griffin.yml
