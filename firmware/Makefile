M68K_GCC = ./m68k-gcc
M68K_OBJCOPY = ./m68k-objcopy

SOURCES = crt0.s rom.cpp syscalls.c build_info.c
LDSCRIPT = linker.ld

rom.bin: rom.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

rom.elf: $(SOURCES) $(LDSCRIPT)
	$(M68K_GCC) -g -T $(LDSCRIPT) $(SOURCES) -o $@

rom.s: rom.cpp $(LDSCRIPT)
	$(M68K_GCC) -S -g rom.cpp

# Courtesy Claude Opus 4.6
build_info.c: FORCE
	@echo '/* auto-generated — do not edit */' > $@
	@echo 'const char *build_date = "'"$$(date '+%Y-%m-%d %H:%M:%S')"'";' >> $@
	@BRANCH=$$(git rev-parse --abbrev-ref HEAD); \
	HASH=$$(git rev-parse --short HEAD); \
	if [ "$$BRANCH" = "HEAD" ]; then \
	    REF="$$HASH"; \
	else \
	    REF="$$BRANCH $$HASH"; \
	fi; \
	if ! git diff --quiet HEAD 2>/dev/null; then \
	    REF="$$REF+"; \
	fi; \
	echo "const char *build_provenance = \"$$REF\";" >> $@

clean:
	rm -f rom.elf rom.bin build_info.c

FORCE:

.PHONY: clean FORCE
