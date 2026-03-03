codegen: griffin.generated.h griffin.generated.inc griffin.generated.ld griffin.generated.vh

griffin.generated.h griffin.generated.inc griffin.generated.ld griffin.generated.vh: griffin.yml codegen.py
	python3 codegen.py griffin.yml

validate:
	python3 -m check_jsonschema --schemafile hw_schema.yml griffin.yml
