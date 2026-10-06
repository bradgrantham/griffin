# Where these sources come from

The C and assembly files in this directory are Digital Research's CP/M-68K
BDOS and CCP (version 1.1/1.2 sources, 1982-1983), as converted to GNU C and
GNU as by roman65536 in https://github.com/roman65536/cp-m-m68k (commit
1b0a983, "GCC CP/M M68k 1.2a").  That conversion renamed the Alcyon-syntax
assembler files to gas syntax, replaced the `swap` and `udiv` assembly helpers
with C (`cpm_swap`, `cpm_udiv`), and added a 68010/68020 privilege-violation
patch in exceptn.s that rewrites `move sr,<ea>` in a user program to `move
ccr,<ea>` and retries it.

Digital Research's CP/M sources are distributed under the open licence Bryan
Sparks (DRI's successor in interest) granted in 2022: "Let this paragraph
represent a right to use, distribute, modify, enhance, and otherwise make
available in a nonexclusive manner CP/M and its derivatives."  The copyright
notices in the files are kept.

## Changes made for Griffin

- Removed every commented-out `printf(... __FUNCTION__ ...)` debugging line
  the conversion had left behind, and restored the `bconstat()` macro in
  biosdef.h (the conversion had replaced it with a static function).
- `bdosmisc.c` `bdosinit()`: the TPA limits come from the BIOS memory region
  table again (`segp->low`, `segp->length`), as in DRI's original; the
  conversion had hard-coded its own board's addresses.
- `bdosif.s`: the BIOS glue copies the result from d0 to a0 before
  returning.  GCC's m68k ABI returns pointers in a0 (with a copy in d0), so
  the C callers of `bios4()` (seldsk) and `bios6()` (getseg), declared as
  returning pointers, read a0; the Alcyon convention was d0 only.
- `ccpload.s`: the RTE that starts a program gets a zero format/vector word
  under the SR and PC, which the 68010 requires (it raised a format error on
  the first program run); a 68000 leaves the word on the system stack, which
  every warm boot resets.  The two `fill_fcb` calls pop 8 bytes, not 6: the
  conversion had widened the pushed word parameter to a long without
  changing the clean-up.
- `stack.s`: the system stack is 16 KB (was 1600 bytes).  CP/M's BDOS, the
  BIOS and through it the ROM's syscalls (FatFs) and the ROM's interrupt
  handlers all run on it.
- Nothing else in the files.  They build as `-std=gnu89 -m68000
  -funsigned-char` (see ../Makefile), with their 1983 warnings silenced.
  `-funsigned-char` matters: bdosinc.h defines `UBYTE` as plain `char`
  (`ALCYON` is defined), and Alcyon C's `char` was unsigned, so
  `create()`'s `dirp->entry == 0xe5` never matched with a signed char and
  every file creation failed with "no directory space".

Not included from the conversion: its `bios/` directory (a different board),
`simbios.s`, `pgmld.s` (the pre-gas copy of pgmld2.s), `bios/ide.c`.

## Known limits of this code on Griffin

- The exception reporter in exceptn.s assumes 68000 stack frames.  On the
  68010 the "at user address" it prints for a bus or address error is wrong
  (the 68010 frame is larger); the warm boot that follows still works, and
  every other exception reports the right address.  BDOS function 61
  (user-installed exception handlers) is likewise 68000-frame only.
- User programs run in user mode.  The privilege-violation patch above is
  what lets 1983 DRI binaries that read SR run on a 68010.
