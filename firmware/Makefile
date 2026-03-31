M68K_GCC = ./m68k-gcc
M68K_OBJCOPY = ./m68k-objcopy

REAL_SOURCES = crt0.s rom.cpp syscalls.c
SOURCES = $(REAL_SOURCES) build_info.c
OBJECTS = startup_audio.o
LDSCRIPT = linker.ld

rom_even.bin rom_odd.bin: rom.bin
	$(M68K_OBJCOPY) -I binary -O binary --byte=0 --interleave=2 $< rom_even.bin
	$(M68K_OBJCOPY) -I binary -O binary --byte=1 --interleave=2 $< rom_odd.bin

flash-even: rom_even.bin
	minipro -s -b -p "M27C512@DIP28" -w rom_even.bin

flash-odd: rom_odd.bin
	minipro -s -b -p "M27C512@DIP28" -w rom_odd.bin


rom.bin: rom.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

# 11025 8-bit mono signed(?)
startup_audio.o: startup.raw
	$(M68K_OBJCOPY) -I binary -O elf32-m68k -B m68k \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents $< $@

rom.elf: $(SOURCES) $(OBJECTS) $(LDSCRIPT)
	$(M68K_GCC) -g -T $(LDSCRIPT) $(SOURCES) $(OBJECTS) -o $@

rom.s: rom.cpp $(LDSCRIPT)
	$(M68K_GCC) -S -g rom.cpp

# Regenerate build_info.c only when sources, git HEAD, or dirty state change.
# Write to a temp file first; replace only if content differs, preserving the
# timestamp when nothing changed so Make won't rebuild downstream.
build_info.c: $(REAL_SOURCES) $(LDSCRIPT)
	@echo '/* auto-generated — do not edit */' > $@.tmp
	@echo 'const char *build_date = "'"$$(date '+%Y-%m-%d %H:%M:%S')"'";' >> $@.tmp
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
	echo "const char *build_provenance = \"$$REF\";" >> $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

clean:
	rm -f rom.elf rom.bin rom_even.bin rom_odd.bin build_info.c build_info.c.tmp startup_audio.o

.PHONY: clean
