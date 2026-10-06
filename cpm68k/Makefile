# CP/M-68K for Griffin.
#
#   make            CPM.BIN, the system (CCP + BDOS + Griffin BIOS) as a flat
#                   Griffin application; the monitor runs it with `cpm`
#   make drives     A.CPM (the CP/M-68K 1.3 utilities from dist/) and an empty
#                   B.CPM, 8 MB each, with tools/cpmdisk.py
#   make stage      copy CPM.BIN and the drives into apps/stage for apps/mkcf.sh
#   make cf         a 32 MB card image with only CP/M on it (cpm-cf.img)
#   make check      objdump CPM.BIN for 68020-only instructions (none allowed)
#   ./check-cpm.sh  boot CP/M in the emulator and exercise it (needs rom.bin)
#
# Toolchain: the apps/ podman wrappers by default (m68k-unknown-elf, newlib,
# unused here: everything is freestanding).  A native cross compiler works
# just as well:  make CROSS=m68k-linux-gnu-
GRIFFIN ?= ..
CROSS   ?= $(GRIFFIN)/apps/m68k-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump
GEN      = $(GRIFFIN)/griffin.generated

STAGE   ?= $(GRIFFIN)/apps/stage
DRIVE_MB ?= 8

# The DRI sources are 1983 K&R C that assumes a 16-bit int in places the GCC
# conversion already papered over; they build as gnu89 with their warnings
# silenced, the BIOS builds strict.  -funsigned-char: Alcyon C's char was
# unsigned and the BDOS compares directory bytes against 0xE5 through it.
MACHFLAGS = -m68000 -g -Os -ffreestanding -fno-builtin -fno-strict-aliasing \
            -fno-asynchronous-unwind-tables -fno-common
SYSCFLAGS = $(MACHFLAGS) -std=gnu89 -funsigned-char -w
BIOSCFLAGS = $(MACHFLAGS) -std=c2x -Wall -Wextra -Werror -Wshadow
ASFLAGS   = -m68000 -g -I$(GRIFFIN)
LDFLAGS   = -m68000 -nostdlib -nostartfiles -T cpm.ld -Wl,--no-warn-rwx-segments

SYS_C = bdosmain.c bdosmisc.c bdosrw.c ccp.c conbdos.c dskutil.c fileio.c iosys.c
SYS_S = bdosif.s ccpbdos.s ccpif.s ccpload.s exceptn.s filetyps.s pgmld2.s stack.s
BIOS_C = bios.c
BIOS_S = biosa.s sys.s libgcc68k.s

SYS_OBJ  = $(addprefix system/,$(SYS_C:.c=.o) $(SYS_S:.s=.o))
BIOS_OBJ = $(addprefix bios/,$(BIOS_C:.c=.o) $(BIOS_S:.s=.o))

all: CPM.BIN

# Regenerate the YAML-derived includes via the root, like apps/app.mk.
$(GEN).%: $(GRIFFIN)/griffin.yml $(GRIFFIN)/codegen.py
	@$(MAKE) --no-print-directory -C $(GRIFFIN) codegen

bios/sys.o: $(GEN).inc
$(SYS_OBJ) $(BIOS_OBJ): Makefile       # flags live here

system/%.o: system/%.c $(wildcard system/*.h)
	$(CC) $(SYSCFLAGS) -c $< -o $@

system/%.o: system/%.s
	$(CC) $(ASFLAGS) -c $< -o $@

bios/%.o: bios/%.c
	$(CC) $(BIOSCFLAGS) -c $< -o $@

bios/%.o: bios/%.s
	$(CC) $(ASFLAGS) -c $< -o $@

# The BIOS object goes first so _start (.text.start) leads the image; the link
# order of the rest follows the DRI build.  No -lgcc: bios/libgcc68k.s has the
# helpers, and a host libgcc built for the 68020 would bring 68020 opcodes.
cpm.elf: $(BIOS_OBJ) $(SYS_OBJ) cpm.ld
	$(CC) $(LDFLAGS) $(BIOS_OBJ) $(SYS_OBJ) -o $@

CPM.BIN: cpm.elf
	$(OBJCOPY) -O binary -S $< $@
	@ls -l $@

# Mnemonics that exist only from the 68020 on; the 68000/68010 trap on them.
# Disassembled as a 68020 so that, e.g., a 32-bit-displacement bsr shows up as
# "bsrl" instead of "bsrs" followed by .short words.
CHECK_68020 = '\b(bral|bsrl|b(cc|cs|eq|ne|ge|gt|hi|le|ls|lt|mi|pl|vc|vs)l|divsl|divul|mulsl|mulul|extbl|bf[a-z]+|cmp2|chk2|pack|unpk|rtd|cas2?|callm|rtm|linkl|movec|moves)\b'
check: cpm.elf
	@if $(OBJDUMP) -d -m68020 $< | grep -E $(CHECK_68020); then \
	    echo "check: 68020-only instructions found"; exit 1; \
	else echo "check: no 68020-only instructions"; fi

# ---- drive images ---------------------------------------------------------
drives: A.CPM B.CPM

CPMDISK = python3 tools/cpmdisk.py

# GRIFFIN.68K: EXIT (back to the monitor) and TIME.  Position-independent
# gas, linked flat at 0, wrapped in the CP/M-68K header by tools/mk68k.py.
tools/griffin.o: tools/griffin.s
	$(CC) $(ASFLAGS) -c $< -o $@

tools/griffin.bin: tools/griffin.o
	$(CC) -m68000 -nostdlib -nostartfiles -Wl,-Ttext=0 -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-e,_start $< -o tools/griffin.elf
	$(OBJCOPY) -O binary -S tools/griffin.elf $@

GRIFFIN.68K: tools/griffin.bin tools/mk68k.py
	python3 tools/mk68k.py $< $@

A.CPM: $(wildcard dist/*) GRIFFIN.68K tools/cpmdisk.py
	$(CPMDISK) create $@ $(DRIVE_MB)
	$(CPMDISK) put $@ dist/* GRIFFIN.68K
	$(CPMDISK) ls $@ | tail -1

B.CPM: tools/cpmdisk.py
	$(CPMDISK) create $@ $(DRIVE_MB)

stage: CPM.BIN drives
	@mkdir -p $(STAGE)
	cp CPM.BIN A.CPM B.CPM $(STAGE)/

# A card with just CP/M on it, for the emulator (check-cpm.sh) or a real card.
CF ?= cpm-cf.img
cf: CPM.BIN drives
	rm -rf cf-stage && mkdir -p cf-stage
	cp CPM.BIN A.CPM B.CPM cf-stage/
	$(GRIFFIN)/apps/mkcf.sh cf-stage $(CF) 32m

clean:
	rm -f cpm.elf CPM.BIN $(SYS_OBJ) $(BIOS_OBJ) A.CPM B.CPM cpm-cf.img \
	      GRIFFIN.68K tools/griffin.o tools/griffin.elf tools/griffin.bin
	rm -rf cf-stage

.PHONY: all check drives stage cf clean
