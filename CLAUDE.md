# Griffin Project

## Intent

* This is a homebrew 68000 computer intended to let me use some parts I've had for decades in my bin, including 68000P12, AS6C4008-55PCN, a bunch of 70ns 27C512, and a handful of discrete parts.  The IC parts bin inventory is at chip-inventory.csv but I also have assorted LEDs, transistors, diodes, and a large selection of capacitors and resistors.  I've added to my collection for this project: MC68681, XR68C681, ATF1508CPLD, W27C512-45Z, 68010P12, 7200 LP15


* Goals include display of bitmaps starting with 640*480*1bpp, audio, and CF reading.  Possibly FUZIX, MiNT, EmuTOS, CP/M-68K, Linux

## Definition

* griffin.yml is source of design
  * Original project notes in griffin.md but no need to read unless context required or to edit
  * KICAD rev 1 board design is in board/
    * A print of the PCB and schematic are in board_rev_1_pcb.pdf and board_rev_1_schematic.pdf
    * netlist is in board/board-pcb-rev-1.distilled.txt except with bodges applied as noted in griffin.yml, produced with kicad_netlist_summary_2.py
    * Rev 1 gerbers are in board/board-gerb
  * GLUE Verilog for ATF1508AS in cpld/glue
  * What was an AT89S51 in the board rev and in the distilled netlist is now a XR68C681 DUART on a bodged DIP carrier.
  * Makefile for GLUE, VIDEO, ENGINE in cpld
  * ROM in firmware/{crt0.s,linker.ld,rom.cpp,Makefile} and associated other files in firmware/
  * bringup ROM in sanity/{sanity.s,linker.ld,Makefile} (not keeping up to date)
  * emulator in emulator/ and the intent is to at least emulate the 68k and MMIO accesses.  TBD whether to emulate the ATF1508's using Verilator.

* When possible, store new hardware definitions in griffin.yml; register addresses, bits and bitfields, constants, protocol between peripherals, constants, and then generate included headers.
  * In a register `description:`, keep the **first sentence on one physical line ending in a period**, with no mid-text periods in it (e.g. avoid `FOO.BAR`).  codegen.py uses that first sentence verbatim — newlines included — as the C header `//` comment, so a wrapped first sentence leaks bare text into griffin.generated.h/.refs.h and breaks the build.  (The .inc/.vh outputs only emit the access keyword, so only the C++ headers are sensitive.)

* Keep in mind for instruction-counted loops that there are ROM wait states.

* The pins have been hand-assigned to ATF1508 pins, and those must remain where assigned because a PCB has already been manufactured.

* I don't have "timeout", use a perl one-liner instead.

### Hardware and Software balance

* ATF1508 CPLD's are best at deterministic behavior, parallel processing, and high-speed response/signaling, but have limited real-estate so functionality must be kept as minimal as possible
* The CPU is configurable and flexible but instructions take variable time, flow may be stalled by DTACK and interrupts, and real-time response is difficult.
* Video DMA stalls and jitters CPU timing (DTACK + FIFO refill), so anything hard-real-time must be device-clocked or live in a dedicated peripheral — never a CPU bit-loop or a per-bit ISR that has to `rte` in time for the next edge.
* Therefore carefully split responsibility between the CPLDs and CPU. Examples:
  * A complete UART RX and TX doesn't fit in GLUE, so serial uses the 68681 DUART (its own baud generator + FIFO make byte timing independent of CPU stalls).  (An earlier GLUE "TIMER" that stalled DTACK for bit-bang serial was removed because video DMA broke its timing.)
  * PS/2 is a GLUE frame engine that assembles a whole frame and raises one IRQ per byte (and shifts TX out on the device clock), rather than a per-bit IRQ the CPU could miss under DMA.
  * Rather than encoding progressive versus interlaced DMA, just have a "row stride" that the CPU can set and also add once in the video blank ISR to set up field 1. (for future video)
* Prefer C++23 with correct idioms when possible, C when it makes more sense than C++, use assembly when the code must be in assembly.

## Building components

* Generate C++, Verilog, and assembly includes for components with `make` at project root.  After editing griffin.yml, run this first — before building firmware/CPLD/emulator — since all three consume the generated files.
* Build glue and other CPLD Verilog in cpld/ with `make {thing}`; {edif,fit,io,jed,pin,svf,tt3} files are outputs.  When planning, ignore the outputs (especially .fit)
* Configure emulator CMake in emulator/ with `cmake -Bbuild .` and build with `cmake --build build`
* Build the ROM in firmware/ with `make`.  The toolchain is made from a Docker image of an Ubuntu 24 build of crosstool-ng for m68k-unknown-elf for 68000 and not for 68832; see firmware/m68k-crosstool-ng.config, firmware/m68k-{g++,gcc,objcopy,objdump}, BUILD_TOOLCHAIN_CONTAINER, Dockerfile.  The toolchain .tar.gz might not be in git.
* Build the sanity test ROM image in firmware/ with `make`.  Same toolchain as firmware/.  (Probably should unify the toolchains between firmware and rom at some point...)

## Testing components

* If changing Verilog, verify it fits the CPLD (or improves utilization if that's the task) before making other source changes.  If the change doesn't fit there's no point in updating the source.

### Video-chain discipline (established Aug 2026; violating it has found real bugs every time it was applied)

* **The propagation lockstep.**  For the video chain, five layers must agree and a semantic RTL change is INCOMPLETE until propagated through all of them: (1) the RTL (cpld/{compositor,pixel}/), whose header comments state the semantic laws; (2) the chip testbench (compositor_tb.v, pixel_tb.v — `make compositor-sim` / `make pixel-sim`), which is the executable timing spec and whose MEASURED CONSTANTS are ground truth; (3) the shared model super-engine/render.{h,cpp} (PixelUnit/CompositorUnit), kept equation-for-equation with the RTL; (4) the suite cases (super-engine/main.cpp) asserting authored screens pixel-exact; (5) the emulator, which inherits through the shared render.cpp and must rebuild clean.
* **Testbench rules**: every video CPLD gets a testbench (compositor_tb.v, pixel_tb.v, and since 2026-08-26 timing_tb.v — `make timing-sim` — which checks every TIMING output against closed-form laws on every clock plus output-to-output relations, and runs `-DMUTATE=n` negative controls that must fail).  Expectations are hand-derived from the documented laws BEFORE simulating, never tuned to sim output; a derivation-vs-sim disagreement halts for cycle-by-cycle analysis; include negative-control mutations proving the bench can fail.  pixel_tb.v found two shipped-RTL bugs (bit_pos drift, PIX_LAST tail) on the day it was created.
* **Pinouts are FROZEN** (//PIN: blocks: GLUE+PORTS 2026-07-30; COMPOSITOR+PIXEL+TIMING 2026-08-26).  Route the board only from those blocks.  All fitter targets use `-preassign keep` plus the production strategy flags (JTAG = on, TDI/TMS pullups, power_reset) — never fit a to-be-frozen design without them.  Adding NEW ports may place on spare pins (append them to the block); MOVING a frozen pin is a board respin.  TIMING has 21 spare I/O; new-pin features are bitfile-only ONLY if the schematic routes those spares to reachable copper.
* **Fit experiments** run as ladders with every rung's LC/FF/PT recorded, and rejected alternatives are kept as documented negative results (pixel_combined.v, the 4-stage delay line, the planar mask, the inline-color MASK header).
* **The datasheet** griffin-video-datasheet.html (also published as a Claude artifact) is the mechanism-and-recipes reference: record encodings, measured seam/cadence constants, screen-mode budgets, and qualification status.  Update its status stamps (PRODUCTION / PRELIMINARY / OPEN) when packages land.
* sanity/sanity.bin and firmware/rom.bin should execute in emulator/emulator/build/emulator.
  * note that by default UART TX and RX is through a PTY and video is an SDL window — both are interactive and awkward to drive unattended.

## General design guidelines

* Prefer correction by construction, but not through unreasonable complexity.

## Claude Code Guidelines

* Prefer C++23 for host-based tools and bare-metal-capable C++23 for the 68000 firmware.
* When writing C++, use the design guidelines in c++-style.md
* When writing Verilog, use the design guidelines in verilog-style.md

### Design

* In general prefer facilities don't cross-communicate except absolutely necessary.  E.g. a CF card facility can fill in a string with identity, but wouldn't call the UART to print it.  A higher function would call to get the identity, and then call whatever routine it prefers to print the identity or store it in NVRAM or whatever.

