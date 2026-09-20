# Build the 'dmabench' Griffin application into a flat binary loadable at 0x1000.
#
# Every build product, including the copies of the apps/lib support objects,
# lands in THIS directory: nothing outside apps/dmabench/ is written.  That is
# why this Makefile carries its own rules instead of including apps/lib/lib.mk,
# which compiles into apps/lib/.
#
# Every flag is assigned with = or := so that an exported CXXFLAGS in the
# invoking shell cannot leak into the target build.

GRIFFIN := ../..
APPS    := $(GRIFFIN)/apps
LIB     := $(APPS)/lib
GEN     := $(GRIFFIN)/griffin.generated

export GRIFFIN_MOUNT_ROOT := $(abspath $(GRIFFIN))

M68K_GCC     := $(APPS)/m68k-gcc
M68K_GXX     := $(APPS)/m68k-g++
M68K_OBJCOPY := $(APPS)/m68k-objcopy

WARNFLAGS   := -Wall -Wextra -Werror
COMMONFLAGS := -g -O2 -fno-omit-frame-pointer
INCLUDES    := -I$(LIB) -I$(GRIFFIN)
CFLAGS      := --std=c23 $(COMMONFLAGS) $(WARNFLAGS) $(INCLUDES)
CXXFLAGS    := --std=c++23 $(COMMONFLAGS) $(WARNFLAGS) $(INCLUDES)
ASFLAGS     := -g
LDFLAGS     := -T $(LIB)/app.ld

# crt0.o must stay first: app.ld puts .text.start at the load base and asserts
# on it.
OBJECTS := crt0.o syscalls.o griffin_video.o griffin_input.o dmabench.o

all: dmabench.bin

$(GEN).%: $(GRIFFIN)/griffin.yml $(GRIFFIN)/codegen.py
	@$(MAKE) --no-print-directory -C $(GRIFFIN) codegen

$(OBJECTS): $(GEN).h $(GEN).inc

# The apps/lib support objects, compiled into this directory.
crt0.o: $(LIB)/crt0.s
	$(M68K_GCC) $(ASFLAGS) -c $< -o $@

syscalls.o: $(LIB)/syscalls.cpp $(LIB)/griffin_app.h $(LIB)/griffin_syscall.h
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

griffin_video.o: $(LIB)/griffin_video.cpp $(LIB)/griffin_video.h $(LIB)/griffin_syscall.h \
                 $(GRIFFIN)/griffin_abi.h
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

griffin_input.o: $(LIB)/griffin_input.cpp $(LIB)/griffin_input.h
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

dmabench.o: dmabench.cpp $(GRIFFIN)/super-engine/descriptor.h $(GRIFFIN)/griffin_abi.h \
            $(LIB)/griffin_video.h $(LIB)/griffin_app.h
	$(M68K_GXX) $(CXXFLAGS) -c $< -o $@

dmabench.elf: $(OBJECTS) $(LIB)/app.ld
	$(M68K_GXX) $(LDFLAGS) $(OBJECTS) -o $@

dmabench.bin: dmabench.elf
	$(M68K_OBJCOPY) -O binary -S $< $@

# A FAT16 superfloppy the emulator's --cf can mount, with DMABENCH.BIN on it.
cf: dmabench.bin
	mkdir -p stage
	cp dmabench.bin stage/DMABENCH.BIN
	$(APPS)/mkcf.sh stage dmabench-cf.img 16m

clean:
	rm -f $(OBJECTS) dmabench.elf dmabench.bin dmabench-cf.img
	rm -rf stage

.PHONY: all cf clean
