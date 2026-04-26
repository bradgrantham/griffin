# Griffin Project

## Intent

* This is a homebrew 68000 computer intended to let me use some parts I've had for decades in my bin, including 68000P12, 2x KM681000BLP-7, a bunch of 70ns 27C512, and a handful of discrete parts.  The IC parts bin inventory is at chip-inventory.csv but I also have assorted LEDs, transistors, diodes, and a large selection of capacitors and resistors.

* Goals include display of bitmaps starting with 640*480*1bpp, audio, and CF reading.  Possibly FUZIX, MiNT, EmuTOS, CP/M-68K

## Definition

* griffin.yml is source of design
  * Original project notes in griffin.md but no need to read unless context required or to edit
  * KICAD rev 1 board design is in board/
    * A print of the PCB and schematic are in board_rev_1_pcb.pdf and board_rev_1_schematic.pdf
    * netlist is in board/board-pcb-rev-1.distilled.txt except with bodges applied as noted in griffin.yml, produced with kicad_netlist_summary_2.py
    * Rev 1 gerbers are in board/board-gerb
  * GLUE Verilog for ATF1508AS in cpld/glue
  * 68681 DUART
  * Makefile for GLUE in cpld
  * 68000 ROM in firmware/{crt0.s,linker.ld,rom.cpp,Makefile} and associated other files in firmware
  * 68000 bringup ROM in sanity/{sanity.s,linker.ld,Makefile}
  * emulator in emulator/ and the intent is to at least emulate the 68000 and MMIO accesses.  TBD whether to emulate the ATF1508's using Verilator.

* When possible, store new hardware definitions in griffin.yml; register addresses, bits and bitfields, constants, protocol between peripherals, constants, and then generate included headers.

* Keep in mind for instruction-counted loops that there are ROM wait states.

* The pins have been hand-assigned to ATF1508 pins, and those must remain where assigned because a PCB has already been manufactured.

* I don't have "timeout", use a perl one-liner instead.

### Hardware and Software balance

* ATF1508 CPLD's are best at deterministic behavior, parallel processing, and high-speed response/signaling, but have limited real-estate so functionality must be kept as minimal as possible
* The 68000 CPU is configurable and flexible but instructions take variable time, flow may be stalled by DTACK and interrupts, and real-time response is difficult.
* Therefore carefully split responsibility between the CPLDs and CPU. Examples:
  * A complete UART RX and TX doesn't fit in GLUE, but GLUE implements a free-running reloaded "TIMER" so that CPU code can have more deterministic hard timing and perform UART RX and TX in a loop.
  * Rather than encoding progressive versus interlaced DMA, just have a "row stride" that the CPU can set and also add once in the video blank ISR to set up field 1. (for future video)

## Building components

* Generate C++, Verilog, and assembly includes for components with `make` at project root.
* Build glue and other CPLD Verilog in cpld/ with `make {thing}`; {edif,fit,io,jed,pin,svf,tt3} files are outputs.  When planning, ignore the outputs (especially .fit)
* Configure emulator CMake in emulator/ with `cmake -Bbuild .` and build with `cmake --build build`
* Build the ROM in firmware/ with `make`.  The toolchain is made from a Docker image of an Ubuntu 24 build of crosstool-ng for m68k-unknown-elf for 68000 and not for 68832; see firmware/m68k-crosstool-ng.config, firmware/m68k-{g++,gcc,objcopy,objdump}, BUILD_TOOLCHAIN_CONTAINER, Dockerfile.  The toolchain .tar.gz might not be in git.
* Build the sanity test ROM image in firmware/ with `make`.  Same toolchain as firmware/.  (Probably should unify the toolchains between firmware and rom at some point...)

## Testing components

* If changing Verilog, verify it fits the CPLD (or improves utilization if that's the task) before making other source changes.  If the change doesn't fit there's no point in updating the source.
* sanity/sanity.bin and firmware/rom.bin should execute in emulator/emulator/build/emulator.
  * note that UART TX and RX is through a PTY.

## General design guidelines

* Prefer correction by construction, but not through unreasonable complexity.

## Claude Code Guidelines

* Prefer C++20 for host-based tools and bare-metal-capable C++20 for the 68000 ROM firmware.
* When writing C++, use the design guidelines in c++-style.md
* When writing Verilog, use the design guidelines in verilog-style.md

### Design

* In general prefer facilities don't cross-communicate except absolutely necessary.  E.g. a CF card facility can fill in a string with identity, but wouldn't call the UART to print it.  A higher function would call to get the identity, and then call whatever routine it prefers to print the identity or store it in NVRAM or whatever.

