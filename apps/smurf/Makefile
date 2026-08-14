# Build the 'smurf' Griffin application into a flat binary loadable at 0x1000.
#
# Same shape as apps/dtest: the direct-video and input support objects come
# from the shared apps/lib fragment.  The sprite span lists are a generated
# header in image-tools/smurf-assets and are included by relative path, so no
# extra include directory is needed; the backdrops themselves are data files
# that the app reads off the CF card at run time.

M68K_GCC     = ../m68k-gcc
M68K_GXX     = ../m68k-g++
M68K_OBJCOPY = ../m68k-objcopy

LIB = ../lib
GEN = ../../griffin.generated
ASSETS = ../../image-tools/smurf-assets

WARNFLAGS = -Wall -Wextra -Werror
COMMONFLAGS = -g -O2 -fno-omit-frame-pointer
CFLAGS   = --std=c23 $(COMMONFLAGS) $(WARNFLAGS)
CXXFLAGS = --std=c++23 $(COMMONFLAGS) $(WARNFLAGS)
ASFLAGS  = -g
LDFLAGS  = -T $(LIB)/app.ld

# Stated before the include: lib.mk's header-dependency rules are explicit
# targets, so whichever of them make sees first would otherwise become the
# default goal and `make` would build one object instead of the app.
all: smurf.bin

include $(LIB)/lib.mk

OBJECTS = $(GRIFFIN_LIB_OBJECTS) smurf.o

# Single source of truth: regenerate the YAML-derived includes via the root
# (same pattern as firmware/Makefile), then rebuild objects that depend on them.
$(GEN).%: ../../griffin.yml ../../codegen.py
	@$(MAKE) --no-print-directory -C ../.. codegen

$(OBJECTS): $(GEN).h $(GEN).inc

smurf.o: smurf.cpp ../../super-engine/descriptor.h ../../griffin_abi.h \
         $(ASSETS)/smurf_sprites.h \
         $(LIB)/griffin_app.h $(LIB)/griffin_video.h $(LIB)/griffin_input.h
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

smurf.elf: $(OBJECTS) $(LIB)/app.ld
	$(M68K_GXX) $(LDFLAGS) $(OBJECTS) -o $@

smurf.bin: smurf.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

clean: griffin-lib-clean
	rm -f smurf.elf smurf.bin smurf.o

.PHONY: all clean
