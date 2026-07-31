# Build the 'exctest' Griffin application into a flat binary loadable at 0x1000.
#
# Adapted from apps/hello: same toolchain wrappers and app.ld, but compiled as
# C++ (exceptions and RTTI stay enabled — apps deliberately omit -fno-exceptions
# and -fno-rtti, unlike the ROM firmware).

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

all: exctest.bin

# Single source of truth: regenerate the YAML-derived includes via the root
# (same pattern as firmware/Makefile), then rebuild objects that depend on them.
$(GEN).%: ../../griffin.yml ../../codegen.py
	@$(MAKE) --no-print-directory -C ../.. codegen

$(OBJECTS): $(GEN).h $(GEN).inc

main.o: main.cpp
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

$(LIB)/syscalls.o: $(LIB)/syscalls.cpp
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

$(LIB)/crt0.o: $(LIB)/crt0.s
	$(M68K_GCC) $(ASFLAGS) -c $< -o $@

exctest.elf: $(OBJECTS) $(LIB)/app.ld
	$(M68K_GXX) $(LDFLAGS) $(LIB)/crt0.o main.o $(LIB)/syscalls.o -o $@

exctest.bin: exctest.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

clean:
	rm -f exctest.elf exctest.bin main.o $(LIB)/crt0.o $(LIB)/syscalls.o

.PHONY: all clean
