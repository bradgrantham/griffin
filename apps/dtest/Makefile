# Build the 'dtest' Griffin application into a flat binary loadable at 0x1000.
#
# Unlike hello/exctest/basic this app uses the shared apps/lib build fragment,
# so the direct-video and input support objects come from one place.

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

# Stated before the include: lib.mk's header-dependency rules are explicit
# targets, so whichever of them make sees first would otherwise become the
# default goal and `make` would build one object instead of the app.
all: dtest.bin

include $(LIB)/lib.mk

OBJECTS = $(GRIFFIN_LIB_OBJECTS) dtest.o

# Single source of truth: regenerate the YAML-derived includes via the root
# (same pattern as firmware/Makefile), then rebuild objects that depend on them.
$(GEN).%: ../../griffin.yml ../../codegen.py
	@$(MAKE) --no-print-directory -C ../.. codegen

$(OBJECTS): $(GEN).h $(GEN).inc

dtest.o: dtest.cpp ../../super-engine/descriptor.h ../../griffin_abi.h \
         $(LIB)/griffin_video.h $(LIB)/griffin_input.h
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

dtest.elf: $(OBJECTS) $(LIB)/app.ld
	$(M68K_GXX) $(LDFLAGS) $(OBJECTS) -o $@

dtest.bin: dtest.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

clean: griffin-lib-clean
	rm -f dtest.elf dtest.bin dtest.o

.PHONY: all clean
