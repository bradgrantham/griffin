M68K_GCC = ./m68k-gcc
M68K_GXX = ./m68k-g++
M68K_OBJCOPY = ./m68k-objcopy

CPP_SOURCES = rom.cpp ps2.cpp
# splash.cpp
C_SOURCES = syscalls.c ff.c diskio.c build_info.c
ASM_SOURCES = crt0.s

REAL_SOURCES = $(ASM_SOURCES) $(CPP_SOURCES) $(filter-out build_info.c,$(C_SOURCES))
SOURCES = $(ASM_SOURCES) $(CPP_SOURCES) $(C_SOURCES)
OBJECTS = $(ASM_SOURCES:.s=.o) $(CPP_SOURCES:.cpp=.o) $(C_SOURCES:.c=.o)
EXTRA_OBJECTS = # startup_audio.o
LDSCRIPT = linker.ld

WARNFLAGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
COMMONFLAGS = -O -g -fno-omit-frame-pointer # -fsanitize=address,undefined
CXXFLAGS = --std=c++23 $(COMMONFLAGS) $(WARNFLAGS) -Wold-style-cast
CFLAGS = $(COMMONFLAGS) $(WARNFLAGS)
ASFLAGS = -g
LDFLAGS = $(COMMONFLAGS) -T $(LDSCRIPT)

# Third-party C sources we don't control: build with relaxed warning flags
LENIENT_C_SOURCES = ff.c diskio.c
LENIENT_CFLAGS = $(COMMONFLAGS) -Wall
$(LENIENT_C_SOURCES:.c=.o): CFLAGS = $(LENIENT_CFLAGS)

%.o: %.cpp
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

%.s: %.cpp
	$(M68K_GXX) $(CXXFLAGS) -S $<

%.o: %.c
	$(M68K_GCC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(M68K_GCC) $(ASFLAGS) -c $< -o $@

rom_even.bin rom_odd.bin: rom.bin
	$(M68K_OBJCOPY) -I binary -O binary --byte=0 --interleave=2 $< rom_even.bin
	$(M68K_OBJCOPY) -I binary -O binary --byte=1 --interleave=2 $< rom_odd.bin

flash-even: rom_even.bin
	minipro -s -b -p "W27C512@DIP28" -w rom_even.bin

flash-odd: rom_odd.bin
	minipro -s -b -p "W27C512@DIP28" -w rom_odd.bin


rom.bin: rom.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

# 11025 8-bit mono signed(?)
startup_audio.o: startup.raw
	$(M68K_OBJCOPY) -I binary -O elf32-m68k -B m68k \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents $< $@

rom.elf: $(OBJECTS) $(EXTRA_OBJECTS) $(LDSCRIPT)
	$(M68K_GXX) $(LDFLAGS) $(OBJECTS) $(EXTRA_OBJECTS) -o $@

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
	rm -f rom.elf rom.bin rom_even.bin rom_odd.bin build_info.c build_info.c.tmp startup_audio.o $(OBJECTS)

.PHONY: clean
