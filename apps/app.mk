# Shared build for a Griffin application, inside or outside this repository.
#
# An app Makefile is:
#
#     GRIFFIN ?= ../..                 # path to the griffin checkout
#     APP = hello
#     APP_OBJECTS = main.o
#     include $(GRIFFIN)/apps/app.mk
#
# Sources are compiled by suffix (.c, .cpp, .s) with -I$(LIB) -I$(GRIFFIN), so
# headers are included as "griffin_app.h", "griffin.generated.h" and
# "super-engine/descriptor.h".  An app may add header dependencies for its
# own objects after the include.
#
# The toolchain wrappers run in a container that mounts one directory.  An app
# outside this repository must set GRIFFIN_MOUNT_ROOT to a common ancestor of
# itself and the griffin checkout (sibling repositories: their parent).  The
# default here, the repo root, is right for apps under apps/.
APPS = $(GRIFFIN)/apps
LIB  = $(APPS)/lib
GEN  = $(GRIFFIN)/griffin.generated
export GRIFFIN_MOUNT_ROOT ?= $(abspath $(GRIFFIN))

M68K_GCC     = $(APPS)/m68k-gcc
M68K_GXX     = $(APPS)/m68k-g++
M68K_OBJCOPY = $(APPS)/m68k-objcopy

WARNFLAGS   = -Wall -Wextra -Werror
COMMONFLAGS = -g -O2 -fno-omit-frame-pointer
INCLUDES    = -I$(LIB) -I$(GRIFFIN)
CFLAGS   = --std=c23 $(COMMONFLAGS) $(WARNFLAGS) $(INCLUDES)
CXXFLAGS = --std=c++23 $(COMMONFLAGS) $(WARNFLAGS) $(INCLUDES)
ASFLAGS  = -g
LDFLAGS  = -T $(LIB)/app.ld

# Stated before the include: lib.mk's header-dependency rules are explicit
# targets, so whichever of them make sees first would otherwise become the
# default goal.
all: $(APP).bin

include $(LIB)/lib.mk

OBJECTS = $(GRIFFIN_LIB_OBJECTS) $(APP_OBJECTS)

# Single source of truth: regenerate the YAML-derived includes via the root
# (same pattern as firmware/Makefile), then rebuild objects that depend on them.
$(GEN).%: $(GRIFFIN)/griffin.yml $(GRIFFIN)/codegen.py
	@$(MAKE) --no-print-directory -C $(GRIFFIN) codegen

$(OBJECTS): $(GEN).h $(GEN).inc

%.o: %.cpp
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(M68K_GCC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(M68K_GCC) $(ASFLAGS) -c $< -o $@

$(APP).elf: $(OBJECTS) $(LIB)/app.ld
	$(M68K_GXX) $(LDFLAGS) $(OBJECTS) -o $@

$(APP).bin: $(APP).elf
	$(M68K_OBJCOPY) -O binary -S $< $@

clean: griffin-lib-clean
	rm -f $(APP).elf $(APP).bin $(APP_OBJECTS)

.PHONY: all clean
