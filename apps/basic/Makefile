# Build the 'basic' Griffin application into a flat binary loadable at 0x1000.
#
# Adapted from apps/exctest: same toolchain wrappers and app.ld, compiled as C++
# with exceptions and RTTI enabled (the interpreter reports every error by
# throwing).  $(LIB) is on the include path ahead of the system headers so that
# apps/lib/{dirent.h,poll.h,unistd.h} shadow newlib's stubs.

M68K_GCC     = ../m68k-gcc
M68K_GXX     = ../m68k-g++
M68K_OBJCOPY = ../m68k-objcopy

LIB = ../lib
GEN = ../../griffin.generated

WARNFLAGS = -Wall -Wextra -Werror
COMMONFLAGS = -g -O2 -fno-omit-frame-pointer -I$(LIB)
CFLAGS   = --std=c23 $(COMMONFLAGS) $(WARNFLAGS)
CXXFLAGS = --std=c++23 $(COMMONFLAGS) $(WARNFLAGS)
ASFLAGS  = -g
LDFLAGS  = -T $(LIB)/app.ld

OBJECTS = $(LIB)/crt0.o $(LIB)/syscalls.o basic.o

all: basic.bin

# Single source of truth: regenerate the YAML-derived includes via the root
# (same pattern as firmware/Makefile), then rebuild objects that depend on them.
$(GEN).%: ../../griffin.yml ../../codegen.py
	@$(MAKE) --no-print-directory -C ../.. codegen

$(OBJECTS): $(GEN).h $(GEN).inc

basic.o: basic.cpp
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

$(LIB)/syscalls.o: $(LIB)/syscalls.cpp
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

$(LIB)/crt0.o: $(LIB)/crt0.s
	$(M68K_GCC) $(ASFLAGS) -c $< -o $@

basic.elf: $(OBJECTS) $(LIB)/app.ld
	$(M68K_GXX) $(LDFLAGS) $(LIB)/crt0.o basic.o $(LIB)/syscalls.o -o $@

basic.bin: basic.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

clean:
	rm -f basic.elf basic.bin basic.o $(LIB)/crt0.o $(LIB)/syscalls.o

.PHONY: all clean
