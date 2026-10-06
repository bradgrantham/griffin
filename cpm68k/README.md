# CP/M-68K for Griffin

CP/M-68K runs on Griffin as an application: `CPM.BIN` is a flat binary the
ROM monitor loads at 0x1000, and typing `cpm` at the monitor prompt starts it.
It uses the ROM for everything below it, so CP/M gets the VT102 screen, the
PS/2 keyboard and DUART channel A at once, and its drives are ordinary files
on the FAT card.

```
monitor> cpm
CP/M-68K for Griffin: console via ROM, drives A.CPM..D.CPM on the FAT card
A>dir
A>stat
A>pip b:=a:readme.txt
A>griffin exit            (back to the monitor)
```

## What is where

| Directory | Contents |
|---|---|
| `system/` | DRI's BDOS and CCP, GCC-buildable.  See `system/PROVENANCE.md`. |
| `bios/` | The Griffin BIOS: `biosa.s` (entry, TRAP #3 shim, warm boot), `bios.c` (the BIOS functions), `sys.s` (ROM syscall stubs). |
| `tools/` | `cpmdisk.py` (make and edit drive images), `griffin.s` + `mk68k.py` (the GRIFFIN.68K utility), `diskdefs` (the same geometry for cpmtools). |
| `dist/` | The CP/M-68K 1.3 utilities, libraries and C headers from DRI's distribution (PIP, STAT, ED, DDT, AS68, LINK68, the C compiler, ...), in their relocatable form so they load anywhere in the TPA. |
| `cpm.ld` | Linker script: load at 0x1000, TPA from the end of .bss to 0x380000. |

## Building

```
make                        # CPM.BIN, with the podman m68k-unknown-elf wrappers
make CROSS=m68k-linux-gnu-  # or with any m68000-capable cross gcc
make check                  # no 68020-only instructions in CPM.BIN
make drives                 # A.CPM (dist/ + GRIFFIN.68K) and an empty B.CPM, 8 MB each
make stage                  # copy all three into apps/stage; apps/mkcf.sh makes the card
```

The ROM must carry the console character mode and the FSYNC syscall
(griffin.yml SYS_CONSOLE_RAW mode 2, SYS_FSYNC; firmware from 2026-10-05 on).

## How it fits the machine

```
0x000000  RAM vectors.  CP/M installs its handlers for vectors 2..23 and 32..46
          through the BIOS; the BIOS refuses 47 (TRAP #15, the ROM syscall)
          and 24..31 (the ROM's interrupt handlers), and remembers what it
          replaced so `griffin exit` can put the ROM's vectors back.
0x001000  CPM.BIN: CCP, BDOS, BIOS, their data and bss (16 KB system stack,
          allocation vectors), about 50 KB in all
          TPA: from the end of CPM.BIN's bss to 0x380000, about 3.5 MB
0x380000  the firmware's own RAM, heap and supervisor stack; 0x3F0000 the
          ENGINE display list.  Untouched.
```

Console: BIOS conin/conout/const are SYS_READ/SYS_WRITE/SYS_INPUTREADY on the
ROM console in character mode (no echo, no line editing, no newline
translation).  The list device is the console; the punch discards and the
reader returns end-of-file.

Drives: up to four, `A.CPM` to `D.CPM` in the FAT root, opened on first use;
a missing file is an invalid drive.  Each is a CP/M 2.2-style filesystem:
512-byte sectors, 32 per track, 2 KB blocks, 512 directory entries, no skew,
no reserved tracks, 16-bit block numbers, one directory entry per 16 KB.  The
size is the file's size, any whole number of 2 KB blocks from 32 KB to 64 MB.
The BIOS blocks and deblocks the BDOS's 128-byte records through one sector
cache with write-through; BIOS flush (and every warm boot) calls SYS_FSYNC.

On the host, `tools/cpmdisk.py` creates, lists, fills and reads the images
(`create`, `ls`, `put`, `get`, `rm`), with nothing but python3.
`tools/diskdefs` describes the same geometry for cpmtools (`-f griffin8`),
but the Ubuntu 24.04 cpmtools package (2.23) corrupts its heap on these
images once a few files are on them, which is why the script exists; other
cpmtools builds are untested.

Griffin additions to the BIOS (reached through BDOS function 50, the direct
BIOS call): 30 exit to the monitor, 31 seconds since the Unix epoch
(SYS_GETTIME), 32 milliseconds since boot (SYS_GETTICKS).  `GRIFFIN.68K` on
drive A exposes them as `GRIFFIN EXIT` and `GRIFFIN TIME`; `GRIFFIN SR`
reads the status register from user mode, which on the 68010 goes through
the privilege-violation patch that DRI's own binaries need.

## Writing programs

The DRI toolchain on drive A works as it did in 1985: `AS68`, `LINK68`,
`C068`/`C168`/`CP68` with `CLINK.SUB`, `RELOC`.  For a program built on the
host, `tools/griffin.s` + `tools/mk68k.py` show the cheapest route: gas
assembly with PC-relative addressing only, linked flat at 0, wrapped in the
CP/M-68K header with an all-zero relocation map.  A general ELF-to-.68K
converter that emits real relocation entries would let gcc C programs run
too; that is not written yet.

## Limits and notes

- CP/M-68K has no clock calls and no file timestamps; `GRIFFIN TIME` is the
  only clock.
- On the 68010 the exception message for a bus or address error prints the
  wrong address (68000 frame layout assumed by DRI's handler); the warm boot
  after it works.  DRI binaries that read SR run through the
  privilege-violation patch in `system/exceptn.s`.
- Seeks: the ROM's FatFs has no fast-seek table, so a backward seek walks the
  file's cluster chain from the start.  CP/M seeks back to the directory
  constantly, so make the card with 4 KB or larger clusters (`apps/mkcf.sh`
  does).
- The ROM keeps four files open at most, hence four drives.
