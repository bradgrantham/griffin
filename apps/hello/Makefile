# Build the 'hello' Griffin application into a flat binary loadable at 0x1000.
#
# Uses the location-independent toolchain wrappers in apps/ so relative paths
# resolve inside the container the same as on the host.

M68K_GCC     = ../m68k-gcc
M68K_GXX     = ../m68k-g++
M68K_OBJCOPY = ../m68k-objcopy

LIB = ../lib
GEN = ../../griffin.generated

WARNFLAGS = -Wall -Wextra -Werror
COMMONFLAGS = -g -O2 -fno-omit-frame-pointer
CFLAGS   = --std=c23 $(COMMONFLAGS) $(WARNFLAGS)
CXXFLAGS = --std=c++23 $(COMMONFLAGS) $(WARNFLAGS)
ASFLAGS  = -g
LDFLAGS  = -T $(LIB)/app.ld

OBJECTS = $(LIB)/crt0.o $(LIB)/syscalls.o main.o

all: hello.bin

# Single source of truth: regenerate the YAML-derived includes via the root
# (same pattern as firmware/Makefile), then rebuild objects that depend on them.
$(GEN).%: ../../griffin.yml ../../codegen.py
	@$(MAKE) --no-print-directory -C ../.. codegen

$(OBJECTS): $(GEN).h $(GEN).inc

main.o: main.c
	$(M68K_GCC) $(CFLAGS) -c $< -o $@

$(LIB)/syscalls.o: $(LIB)/syscalls.cpp
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

$(LIB)/crt0.o: $(LIB)/crt0.s
	$(M68K_GCC) $(ASFLAGS) -c $< -o $@

hello.elf: $(OBJECTS) $(LIB)/app.ld
	$(M68K_GXX) $(LDFLAGS) $(LIB)/crt0.o main.o $(LIB)/syscalls.o -o $@

hello.bin: hello.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

clean:
	rm -f hello.elf hello.bin main.o $(LIB)/crt0.o $(LIB)/syscalls.o

.PHONY: all clean
