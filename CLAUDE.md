# Griffin Project

## Intent

* This is a homebrew 68000 computer intended to let me use some parts I've had for decades in my bin, including 68000P12, 2x KM681000BLP-7, a bunch of 70ns 27C512, and a handful of discrete parts.  The IC parts bin inventory is at chip-inventory.csv but I also have assorted LEDs, transistors, diodes, and a large selection of capacitors and resistors.

* Goals include but do not require and are not limited to, in rough increasing order of difficulty:
  * FUZIX
  * Running System 3 binaries without memory protection or paging
  * CP/M-68K
  * Display bitmaps of various resolutions, starting with 720*480*1bpp in NTSC up to 640*480*8bpp VGA
  * MiNT variants and EmuTOS

## Definition

* The project is defined by griffin.md and griffin.yml and the KiCAD board design follows closely but is about as authoritative.
  * KICAD board design is in board/ but has progressed significantly from rev 1.
    * A print of the PCB and schematic of Rev 1 are in board_rev_1_pcb.pdf and board_rev_1_schematic.pdf
    * The PCB Rev 1 netlist is in board/board-pcb-rev-1.distilled.txt except with bodges applied as noted in griffin.yml, produced with kicad_netlist_summary_2.py
    * Rev 1 gerbers are in board/board-gerb
  * GLUE Verilog in cpld/glue
  * VIDEO Verilog in cpld/video
  * Makefile for both GLUE and VIDEO in cpld
  * 68000 ROM in firmware/{crt0.s,linker.ld,rom.cpp,Makefile} and associated other files in firmware
  * 68000 bringup ROM in sanity/{sanity.s,linker.ld,Makefile}
  * IO_MCU firmware in io-mcu/{Makefile,main.c} and generated Python script to flash over SPI in io-mcu/at89s52_isp.py
  * emulator in emulator/ and the intent is to at least emulate the 68000 and MMIO accesses.  TBD whether to emulate the ATF1508's using Verilator or emulate the AT89S52 itself.

* When possible, store new hardware definitions in griffin.yml; register addresses, bits and bitfields, constants, protocol between 68000 and IO-MCU, etc, and generate included headers.

* Keep in mind for instruction-counted loops that there are ROM wait states.

* The pins have been hand-assigned to ATF1508 pins, and those must remain where assigned because a PCB has already been manufactured.

* I don't have "timeout", use a perl one-liner instead.

## Building components

* Generate C++, Verilog, and assembly includes for components with `make` at project root.
* Build glue and video in cpld/ with `make glue` or `make video` or `make` (for both)
* Configure emulator CMake in cpld/ with `cmake -Bbuild .` and build with `cmake --build build`
* Build the ROM in firmware/ with `make`.  The toolchain is made from a Docker image of an Ubuntu 24 build of crosstool-ng for m68k-unknown-elf for 68000 and not for 68832; see firmware/m68k-crosstool-ng.config, firmware/m68k-{g++,gcc,objcopy,objdump}, BUILD_TOOLCHAIN_CONTAINER, Dockerfile.  The toolchain .tar.gz might not be in git.
* Build the sanity test ROM image in firmware/ with `make`.  Same toolchain as firmware/.  (Probably should unify the toolchains between firmware and rom at some point...)
* Build the IO MCU firmware in io-mcu/ with `make`, requires `sdcc`.

## Testing components

* sanity/sanity.bin and firmware/rom.bin should execute in emulator/emulator/build/emulator.
  * note that IO-MCU UART TX and RX is through a PTY.

## Claude Code Guidelines

* Prefer C++20 for host-based tools and bare-metal-capable C++20 for the 68000 ROM firmware.

### Design

* In general prefer facilities don't cross-communicate except absolutely necessary.  E.g. a CF card facility can fill in a string with identity, but wouldn't call the UART to print it.  A higher function would call to get the identity, and then call whatever routine it prefers to print the identity or store it in NVRAM or whatever.

### C/C++ Style

- All block statements (`if`, `else`, `for`, `while`, `switch`) must use braces, even for single-line bodies.
- Opening braces go on their own line for block statements and function definitions with the exception of empty blocks, e.g. "while(blah) {}" 

```c
if (condition)
{
    do_thing();
}
else
{
    do_other_thing();
}

void foo(void)
{
    bar();
}
```
