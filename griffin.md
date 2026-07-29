# What is it?

Computer related to parts I already have in the bin

Start with baremetal fun, then progress towards full SpareMiNT/FUZIX/Linux-NOMMU/etc until bored
*  [Atari ST Free Operating Systems - Vincent Rivière](https://youtu.be/28ieOWEQXhU?si=ekVV36ixjHvCfm06&t=1301)  

MVP:

* Fuzix or CP/M-68K or NOMMU Linux 68k with an image viewer: around 640x480x1 with 2 R3G3B2 colors, stretch goal change colors per row in a tight CPU loop  (bonus: 640*480p x 4 colors)


Put this on a screen somehow, from Macbeth:

* Tomorrow, and tomorrow, and tomorrow,
  Creeps in this petty pace from day to day,
  To the last syllable of recorded time;
  And all our yesterdays have lighted fools
  The way to dusty death. Out, out, brief candle!
  Life's but a walking shadow, a poor player,
  That struts and frets his hour upon the stage,
  And then is heard no more. It is a tale
  Told by an idiot, full of sound and fury,
  Signifying nothing.

# References

68000 cycle counts - [https://gist.github.com/cbmeeks/e759c7061d61ec4ac354a7df44a4a8f1](https://gist.github.com/cbmeeks/e759c7061d61ec4ac354a7df44a4a8f1)	

https://www.shopmemory.com/product-category/sram/

Use PLD or CPLD devices - **settled on ATF1508 PLCC-84**

  * [ATF1508AS | Microchip Technology](https://www.microchip.com/en-us/product/ATF1508AS) - PLCC-84  
    * Jameco [Socket PLCC 84 Pin Soldertail Through Hole](https://www.jameco.com/z/4000-84D-R-James-Electronics-Socket-PLCC-84-Pin-Soldertail-Through-Hole_2289799.html)   
  * [GitHub - peterzieba/5Vpld: A collection of scripts and tools for Atmel ATF150x and GAL Programmable logic devices, some of the only standing active 5V programmable logic parts still available.](https://github.com/peterzieba/5Vpld)   
  * They have a USB programmer but that fits a 2x5 header and I’ve already put a 1x5 header on the board assuming I’d make my own cable  
  * They have a lot of resources for design and also a Verilog compiler  
  * Cupl can run under Wine on macOS  
  * “Bake a JTAG header into the board. A 2x5 0.1" header is the standard pinout and takes almost no space. Get an FT232H board (Adafruit sells one for ~$15), wire it up, and you've got a programmer that works with OpenOCD.”  
    * [Adafruit FT232H Breakout - General Purpose USB to GPIO, SPI, I2C](https://www.adafruit.com/product/2264)   
    * No, just put a 1x5 header on and wire from ft232h
  * If there’s a .si file for the PLD “cupl.exe” will run that simulation and put outputs in .so  
* Sourcing~~** ~~ATF1508s may be difficult.  May need to stockpile?~~ Plenty at Microchip for now.
  * [https://www.youtube.com/watch?v=LnGaDpGbbjQ](https://www.youtube.com/watch?v=LnGaDpGbbjQ) has a bunch of details on HDL through Microchip's tools and using these devices  
  * Really need to write HDL **before** doing the PCB because some pins may need to move.

# Design philosophy

Try to do something more long-term sustainable that you can pick up and restart more easily  
Find a way to share constants between Verilog, linker.ld, crt0.s, and C++

* Codegen from a master file

How much design file can be in YAML or in Python?  Generate from YAML:

* Constants in Verilog, linker.ld, crt0.s, and C++  
* Check against netlist from KiCAD

# Case and form factor

~~Maybe target a standard case form factor (e.g. Micro-ATX or Mini-ITX) - use your existing old case?~~

**Nah.**  

**Use USB\_C and print a case.**

# Bodges on rev 1 Board

DEBUG\_OUT LED:

* small NPN like a 2N3904 or SOT-23 MMBT3904. Collector to \+5V through the LED and resistor, base to the DEBUG\_OUT pad through a 1K–10K resistor, emitter to ground. The base current is microamps so it won't load the serial line at all, and the LED gets a clean 5V drive independent of your logic levels.  
* You could dead-bug it right across the two pads - body of the transistor sitting on top, legs bent to reach the resistor and LED. A little ugly but perfectly functional for a dev board.
* See also bodges in [griffin.yml](griffin.yml)

# Software

* BIOS looks like this?
  * Make it a useful standalone monitor/ROM environment:  
    - [ ] Simple memory dump commands (monitor)  
    - [ ] S-record or Intel HEX loader over serial  
    - [ ] xmodem?  
    - [ ] Basic trap dispatch (so user code calls ROM services via TRAP \#n rather than hardcoded addresses)  
  * Boot and run CP/M-68K over serial:  
    - [ ] Memory sizing  
    - [ ] Serial console (polled): init, putchar, getchar, status  
    - [ ] Block device: read sector, write sector, select, geometry query  
    - [ ] Boot: cold start, warm start  
  * Timer hardware  
    - [ ] Timer ISR with a tick counter  
    - [ ] GET\_TIME / SET\_TIME (calendar time, seeded at boot or via serial command)  
    - [ ] Optional delay/sleep primitive  
  * GUI  
    - [ ] Video console driver  
    - [ ] Mouse and keyboard input  
* Monitor looks like:  
  * Print hello and revision and date  
  * Print RAM size  
  * Wait for two seconds for interrupt to monitor - commands:  
    * Dir CF card FAT boot partition  
    * Load and boot from boot partition  
    * Receive and boot from serial  
    * (later) Flash MCU from boot partition file  
      * Disable interrupts
      * Drop back to DEBUG\_IN serial output for duration

  * Check CF card boot partition for boot kernel name or information and boot it

# Components

## Power

USB-C power with switch inline between supply and USB-C port

* “two 5.1k pull-downs on both CC pins of your connector” - check for 1A delivery, test this

## Power-on reset and user reset button

DS1233 reset supervisor

## System clock

CPUCLK - 16MHz oscillator, okay to change and reflash GLUE bitfile if clock is changed

## CPU

68000P12, upgradeable to 68EC000-20 or 68010@12 or any 68K in 64-DIP format

* Redo board to slot in a 68030 at some later date if desired

## RTC

Completely forgot from the beginning (no RTC on Rev 1).

* Full up on pins in GLUE and in IO MCU, so not easy to add on Rev 1, and board space may not be available
* **Rev 2:** DS3231 on I²C via the DUART's spare OP/IP pins — costs no GLUE pins and no bus decode.  See the Rev 2 section.

## Address map

0x0 through 0xBFFFFF - RAM addressing but only sockets for 4MB on v1

* 0b0000\_xxxx\_xxxx\_xxxx\_xxxx\_xxx, 0x0X\_XXXX is bank 1  
* 0b0001\_xxxx\_xxxx\_xxxx\_xxxx\_xxx, 0x1X\_XXXX is bank 2  
* 0b0010\_xxxx\_xxxx\_xxxx\_xxxx\_xxx, 0x2X\_XXXX  is bank 3  
* 0b0011\_xxxx\_xxxx\_xxxx\_xxxx\_xxx, 0x3X\_XXXX  is bank 4  
* Fill them in sequentially  
* DTACK responds whether RAM is populated, D will be repeating within partially populated, floating/junk for not-populated  
* ROM overlaid over RAM bank 1 until write to GLUE config register as described elsewhere  
* Handle these RAM cases:  
  * 256K - using the two chips I have now  
  * 1M-4M by MB - populate up to 4 BANKS contiguously with 512K RAMs starting with bank 1  
  * Test procedure  
    * Write to 0xAA55 to 4M - 2, write 0x7733 to 4M - 4, if 4M - 2 == 0xAA55, then 4M  
    * Repeat for 3M, 2M, 1M  
    * Otherwise assume 256K

    See [griffin.yml](griffin.yml) for the complete peripheral address map.

## ROM

128K ROM from 2x 64K

* Simple bootloader  
* Simple shell  
* No video config in ROM  
* Load and run from CF  
* Receive and run over serial  
* Maybe even stick BASIC in there  
* DTACK follows AS by a clock for 12MHz CPU clock or two clocks if 16MHz or 20MHz CPU

## RAM

Initially 256KByte from 2x 128K KM681000BLP-7 SRAM

* Allow upgrade to 4M by populating eight 512KByte SRAM parts (e.g. [AS6C4008-55PCN Alliance Memory, Inc. | Integrated Circuits (ICs) | DigiKey](https://www.digikey.com/en/products/detail/alliance-memory-inc/AS6C4008-55PCN/4234586) ) - x8 is going to be \> $60  
* A20,A21 select between 4 banks of SRAMs  
* Can have incomplete banks but all lower banks must be populated or RAM will be sparse  
* DTACK follows AS by a clock for 12MHz CPU clock or two clocks if 16MHz or 20MHz CPU

## GLUE logic

Dedicated ATF1508 CPLD for:

* Receive RESET - assert HALT while RESET is asserted  
  * After RESET, read HALT as input and do something if asserted, like flash LED  
* Address decode: ~ROM\_SELECT, ~RAM\_BANK\_{n}\_SELECT, ~IO\_SELECT\_MOSI, ~VIDEO\_SELECT, ~CF\_CS0, ~CF\_CS1, ~AUDIO\_LE, ~ENGINE\_SELECT  
* Invert R/~W to output ~R/W  
* Decode ~UDS and ~LDS and R/~W into ~WRITE\_LO and ~WRITE\_HI  
* PS/2 frame engine: GLUE assembles a full 11-bit PS/2 frame in hardware and raises one level-4 IRQ per byte on RX (byte in PS2\_RX\_DATA, parity/framing flags in PS2\_STATUS for firmware to check), and on TX shifts a host-to-device frame out on the device clock (the PS2\_TX\_DATA write presents the start bit and releases CLK; ACK sampled into PS2\_STATUS.TX\_ACK; TX\_DONE IRQ on completion).  Firmware-computed odd parity is carried in PS2\_TX\_DATA address bit 1 (0x09 = parity 0, 0x0B = parity 1).  Half-duplex, one shared shifter.  Replaced an earlier per-bit assist that video DMA could make miss bits.  
* Serial is the 68681 DUART (Channel A, 115200 8N1), not GLUE.  An earlier GLUE\_TIMER (5-bit ÷8 auto-reload that armed a DTACK stall for deterministic bit timing) drove bit-bang UART TX/RX on DEBUG\_OUT/DEBUG\_IN (`timer_putchar`/`debug_getchar_asm`), but video DMA stalls/jitters the CPU and broke the bit loop, so it was removed.  crt0 now brings the DUART up early and all boot/exception/panic prints go through it (stack-free putchar, TXRDY-poll with timeout to LED blink); pre-DUART failures are LED-blink only.  
* System tick is the 68681 C/T (100 Hz, level-5 IRQ), not GLUE.  
* Autovectors: GLUE asserts ~VPA instead of ~DTACK during IACK cycles (FC=111).  Bodge wires freed GLUE pin 75 to CPU ~VPA.
  * 7: VIDEO  
  * 6: ENGINE  
  * 5: IO  
* ROM initially overlaid at 0x0X\_XXXX, RAM bank 1 not selected  
* DTACK generation logic  
  * Count off for internal registers, RAM, ROM, AUDIO, CF, generated from YAML
  * Assume GLUE’s own access and VIDEO and ENGINE are instantaneous?
  * OR with ENGINE\_DTACK, IO\_DTACK to stall until video expansion or io releases bus  
  * AND result with VIDEO\_STALL on data access so any DTACK is blocked until 16-bit VIDEO shift register in CPLD is ready to be loaded  
* BERR after some number of cycles if DTACK not asserted.  Have one timeout counter for BERR for everything else, like 8 cycles, and then crazy long BERR like 256 for IO\_DTACK  
* DEBUG\_OUT  
  * Sets or clears debug LED and test point output  
  * Pre-DUART boot "alive" blink, and TX-timeout/panic LED blink (no longer a UART line)  
* DEBUG_IN
  * Reads test point input  
  * Unused now that serial is the DUART (formerly the bit-bang UART RX input)
* Registers: see [griffin.yml](griffin.yml).

## VIDEO

[Griffin Video Mode Throughput](https://docs.google.com/spreadsheets/d/1jpam0LNxlgqLVfV4WW1wBMNDqXu4QpefYhichfac1WE/edit?usp=sharing)

NTSC, VGA pixel and timing generation - second ATF1508

* CPLD 16-bit shift register clocks out 1 bit, expands to R3G3B2 through internal pair of palette registers  
* Count off hsync and vsync to provide HSYNC and VSYNC signals and exit-VBLANK interrupt (through GLUE)  
* Registers: see [griffin.yml](griffin.yml). All config registers default to 0 (video disabled). Some can be changed at any time but in practice CPU is in a tight pixel loop during visible lines, so changes happen in hblank or vblank.
* Snooped bus user data (FC2:FC0 == 101\) in “slow palette” mode expects 16bits of pixel data (16 pixels) repeated through visible pixels  

## Compact Flash interface

* Only do 8-bit access to ease routing, D0-D7 so only odd addresses  
* Entirely True IDE PIO mode, no interrupts  
* GLUE manages DTACK, will need to hard-code wait states as necessary (7@12MHz, 12@20MHz)  
* Registers: see [griffin.yml](griffin.yml).

## IO processor - PS/2 Keyboard and Mouse, UART, System tick interrupt

For PCB Rev1, serial is the 68681 DUART and PS/2 is a GLUE frame engine (an earlier GLUE-assisted bit-bang on DEBUG_OUT/DEBUG_IN was removed — video DMA broke its CPU-timed bit loop).  Current status:

* *Working* serial via 68681 DUART Channel A, 115200 8N1: interrupt-driven RX + polled TX (`duart_putchar`); crt0 brings it up early so boot/panic output uses it too
  * Need to get to reliable streaming serial so I can send and receive data to some kind of network device e.g. esp32 (the DUART's FIFO + flow control should get there)
* *Working* SYSTICK at 100Hz from the 68681 C/T (level-5 IRQ)
* *Working* PS/2 RX as a GLUE frame engine: one level-4 IRQ per assembled byte (was a fragile per-bit IRQ)
* *Working* PS/2 TX through the same GLUE frame engine (host-to-device, device ACK sampled); firmware-computed parity carried in the PS2_TX_DATA write address

Previous intent: Keyboard, mouse, serial port through 8051-compatible AT89S52

* **Unlikely to work on Rev 1 PCB.**  The AT89S52's P2 port uses weak internal pull-ups (~50 uA) to drive D0-D7, and something on the board is driving the data bus during IO MCU cycles that the pull-ups cannot overcome.  Extensive debugging has ruled out every other chip on the bus (see "Continuing Board Design To-Do" for full debug log).  Communication only worked when the board was running abnormally slowly due to logic analyzer interference with CLK.  For Rev 1, UART is handled by GLUE bit-bang via DEBUG_OUT/DEBUG_IN at 115200 baud.  Rev 2 should add a 74HC245 buffer between AT89S52 P2 and D[7:0], or replace the AT89S52 with a part that has proper bus drivers (e.g. 68681 DUART).

* [AT89S52-24PU Microchip Technology | Integrated Circuits (ICs) | DigiKey](https://www.digikey.com/en/products/detail/microchip-technology/AT89S52-24PU/1008597)  

* 5V UART, just TX, RX, 2 PS2  through GPIO

* AT89S52 Continuously polls IO\_SELECT\_MOSI from GLUE chip: if detected, disable interrupts, do 68000 bus cycle including putting data on data bus, lowering DTACK, then waiting for AS to rise and releasing DTACK, enable interrupts  

* Need FIFO for all inputs so CPU doesn't need to do anything during visible row scanout ISR  

* Registers: see [griffin.yml](griffin.yml).

* Program either in jig or by GLUE control signals  

* ISR for UART, PS2  

* Got that old PS/2 software from PIC for Alice 2  

* I screwed up; kbd and mouse clocks needed to go to P3.2 and P3.3, and I moved them to non-interrupt-capable pins without thinking about it.

* IO MCU doesn't work.  Something on the board is driving the data bus when IO MCU is trying to respond via P2 weak pullups.

  * AT89S52 P2 reads back 0x3C when nothing should be driving D0-D7 (expected 0xFF from weak pull-ups).  0x3C correlates with ROM instruction stream content near current PC, but ROM is verified not driving (see below).
  * Scope shows one device driving D0 to ~5V and another to ~4.5V; MCU P2 weak pull-up (~50µA) cannot overcome whatever is holding D0/D1 low.  Math suggests ~1-2KΩ resistive path to GND would explain all three voltage levels.
  * When board was running abnormally slowly (logic analyzer interference with CLK), IO MCU communication worked correctly including IDENTITY event and string.
  * Verified NOT the cause:
    * ROM: nROM_SELECT (TP14) is HIGH (+5V) during IO MCU cycles; confirmed at both test point and U2 pin 20.
    * RAM: all nRAM_x_SEL confirmed deasserted during IO MCU cycles.
    * CF card: physically removed from connector.
    * Audio 74HC373 (U23): physically pulled from socket.
    * VIDEO CPLD (U17): reflashed with explicit D[15:0] tristate (`assign D = ~nVIDEO_SELECT ? 16'd0 : 16'bz`); no change.
    * ENGINE CPLD (U13): not populated.
    * GLUE address decoding: all chip selects verified mutually exclusive and gated by `bus_cycle`; IO MCU region (0xF8xxxx) cannot overlap any other select.
    * GLUE D bus: tristated unless `glue_read_active` (glue register read), which requires glue_segment (0xF0xxxx), not io_segment (0xF8xxxx).
    * No visible chip select going low during IO MCU cycles on scope.
  * Added 100-iteration NOP delay loop between P2=data and DTACK assertion to extend strong pull-up window; no improvement.

  Move instead to 68681, bus interface is reliable and hardcoded, UART reliable and high-speed with flow control, may be able to do PS/2 through interrupts on input pins

## CPU-driven 8-bit audio

The '373 audio latch is clocked by GLUE's ~AUDIO\_LE on CPU writes to the audio address; there is no hardware FIFO or DMA engine.  Driving the DAC is a CPU timing problem, with two supported patterns:

* **ISR-driven (OS-friendly, ~8-11 kHz).**  The 68681 C/T (or a future VIDEO line IRQ) fires periodically; ISR writes one sample and returns.  (The old GLUE-timer + `play_audio` busywait pacing was removed with the GLUE timer.)  Ceiling is set by ISR overhead on the 14 MHz 68000 with ROM wait states — probably 8-11 kHz before the ISR eats most of the CPU.  Good enough for a general-purpose OS that must also do other work (CP/M-68K, Fuzix).
* **Busywait-driven (game-friendly, up to ~31/15/10 kHz).**  VIDEO exposes a STATUS register whose bit 0 toggles once per visible line (v\_cnt[0]; 31.469 kHz at VGA 640x480@60).  Code polls the toggle, then writes AUDIO.  1x coupling = 31.469 kHz (one sample per flip).  /2 or /3 rate by skipping 1 or 2 lines.  A game that gives up its main loop to audio-plus-framebuffer-writes can spend every non-rendering cycle on audio.

This leaves the VIDEO→U23 AUDIO\_LE bodge (VIDEO pin 36) unused in Rev 1; future revisions may repurpose the pin.

* 8-bit R2R  
* [LM358](https://www.digikey.com/en/products/detail/texas-instruments/LM358P/277042) op-amp  

# Rev 2

## SW Investigation Plan

Clean everything up for Rev 2, get as much tested as possible

* SW improvements
  * graphics routines, take "blit" out of splash.cpp
  * factor out font - should be selectable by enum
* Booter & apps
  * Have a shell in the ROM
  * Image viewer app
  * Movie player app with audio - microham mode
  * BASIC (finish up your basic.cpp)

## Possible new hardware features (July 24 2026)

PS/2 mouse port — to look into: doesn't fit GLUE (grouping failure, every fitter lever tried); would live in ENGINE.  Needs the connector + CLK/DATA nets routed to ENGINE pins at layout time even if the bitfile support comes later.

> **Superseded 2026-07-28:** the mouse does not go in ENGINE either — it gets its own CPLD, PORTS, along with the joysticks, paddles and the audio FIFO pop.  See the PORTS entry below and pcbv2-ports-design.md for the measured fits.

Generalized ENGINE — to look into:

* Stream into VIDEO, AUDIO, or COPY FIFOs from RAM, ROM, CF
* Stream out of COPY FIFO to RAM
* Optional block on HF for VIDEO, AUDIO FIFOs
* Fixed size?  Or count for COPY ops?
* Net impact: as bus master ENGINE reaches the audio FIFO / CF through normal GLUE-decoded cycles, so mostly no new nets — the exception is observing the FIFO HF flags it must block on (AUDIO ~HF at minimum); decide with the "more inter-CPLD signals" layout item

## PORTS — the fourth CPLD (July 28 2026)

Decided after the fit experiment in cpld/ports (full numbers in pcbv2-ports-design.md).  The question was where the PS/2 mouse could go; the answer reshaped the whole small-peripheral end of Rev 2.

What the fits actually said:

* **Two PS/2 frame engines will not share one ATF1508.**  The pair alone — no joystick, no paddle, nothing else — needs 140 of 128 nodes.  Every lever failed identically: `Foldback_logic`, `xor_synthesis`, `Logic_Doubling`, and even TQFP100 (same 128 macrocells, more pins).  A channel is ~50 FF but costs far more than its flip-flops once the foldback expands.  So the keyboard stays in GLUE and the mouse leaves.
* **One channel plus everything else fits with room.**  Mouse + 2 joysticks + 2 paddles + the audio FIFO pop = **111/128 LC, 80 FF, 41/64 I/O, 311 PT**, first pass, no strategy tricks.
* **The 74HCT245 / 74HC590 chips were the wrong answer all along.**  Reading the DE-9s and counting the paddle ramps costs almost nothing in a CPLD that already exists for the mouse: 21 FF for both paddle counters and 0 FF for both joystick ports.  Four DIP packages, the two 74155 read strobes, and the DUART OP4/OP5 dance all go away.
* **A programmable audio divider is not worth a chip.**  That was the whole point of the rev-3 "dedicated ATF1504 for audio" note; it is ~30 FF and it is what pushed every combined fit over the edge.  Halving VIDEO's HSYNC tap costs *one* flip-flop and gives 15.73 kS/s, which is the rate the design already assumed.  One net from VIDEO to PORTS replaces a CPLD.
* **Rev-2 GLUE needs a new pinout.**  The logic fits (107/128 LC) but not with the rev-1 hand-assigned pins kept — routing fails outright.  Fit it with the fitter free, then freeze that placement before routing.  It ends nearly pin-full at 60/64 I/O + 4/4 dedicated inputs.

So Rev 2 is a **four-ATF1508 machine**: GLUE (decode, DTACK, keyboard), VIDEO (VGA timing + micro-HAM), ENGINE (DMA master), PORTS (mouse, sticks, paddles, audio pop).  All four are the same part in the same package, which keeps the bin, the JTAG chain, and the toolchain uniform.

Two reserves worth remembering when the next feature shows up:

* **ENGINE has logic headroom** (~71% used, and it self-decodes its own MMIO so it costs GLUE nothing) — that is where a generalized COPY-FIFO or a second DMA channel goes.
* **PORTS has 23 spare I/O** and 17 spare logic cells — that is where another connector-facing peripheral goes, since it is already the chip that talks to the outside world.

## Strategy

How to reduce chance of spin and reduce likelihood of bodge?  Use golden references, everything else flows from them

Griffin.yml interfaces between chips

* Check netlist against that
* Generate emulator from that

Verilog - uses constants from YAML

* Require fit
* Dictates pin layout of CPLDs

Verify netlist and fit against each other

Maybe an unstructured English definition managed by AI that states machine capabilities and components?  Verify YAML and definition and netlist and fit all against each other?

Verify electrical behavior of all analog components from netlist

Need a rev1 branch for continuing experiments and "main" branch under development is rev2

* Any revelations from rev1 feed into "rev2"

**Rev 1 bodge record**; each entry is either folded into the Rev 2 schematic natively or carried as a design lesson.

| XR68C681P pin | AT89S52 pin or other signal                      |
| ------------- | ------------------------------------------------ |
| A1-A4         | P.0                                              |
| R/WN          | ~WR                                              |
| DTACKN        | P1.6                                             |
| D1-D8         | P2.0-P2.7                                        |
| RxDA          | RXD                                              |
| TxDA          | TXD                                              |
| X1/CLK        | on-carrier 3.6864MHz crystal X1 and 33 pF to GND |
| X2            | on-carrier 3.6864MHz crystal X2 and 33 pF to GND |
| RESETN        | RST                                              |
| CSN           | P1.5                                             |
| IACKN         | VCC                                              |
| VCC           | VCC                                              |
| GND           | GND                                              |

PS/2 CLK and DATA keyboard to GLUE 39 and 40

68000 VPA to GLUE 75

68000 A18 to GLUE 81

Bodging between ENGINE and VIDEO and piggybacked 7200s a la video-fifo-wiring.md but also for both R signals add a 10 pF to GND and 47 Ω at VIDEO (so VIDEO -> 47Ω -> {10 pF -> GND, R})

## Rev 2 components

*(Parts and their function — the Rev 2 spec.  Footprints, nets, placement, and layout gotchas live in Board changes, not here.)*

* **CPU:** 68010 primary (68000 is a drop-in), 14 MHz SYSCLK.
* **Clocking:** 14 MHz system oscillator + a separate 25.175 MHz pixel oscillator for VGA.  SYSCLK feeds a GCLK on each CPLD.
* **Reset:** DS1233 supervisor + user reset button, carried from Rev 1.  ~RESET/~HALT must accept external open-drain assertion from the debug header (backup bus-master role) without fighting the supervisor — the DS1233 tolerates being pulled low externally.
* **RAM:** as Rev 1 — 4 MB as 4 banks × 2 of AS6C4008 (512K×8, DIP-32, socketed, bin parts).  GLUE drives nRAM_[1-4]_SEL (banks by A20/A21) and WRITE_LO/WRITE_HI byte write strobes; /OE←nR_W.  Banks are fully populated, no additional code to manage partial population.
* **ROM:** as Rev 1 — 2× W27C512 (64K×8, DIP-28, socketed, bin parts) = 128 KB on the 16-bit bus, one per byte lane.  Bench-programmed only (no in-circuit write path — firmware iteration is serial/CF load, as Rev 1); nROM_SELECT decode, boot overlay over RAM bank 1 until the CONFIG write; /OE←nR_W.
* **CF:** 16-bit True IDE PIO; IOWR gated by AS (needs a glue.v change: R/~W + AS → IOWR).
* **RTC:** DS3231 (TCXO) on I²C via the DUART's spare OP/IP pins — off the bus, zero GLUE logic (no A17, no chip-select/strobes/wait-states), 5 V so no level translation.  SCL = an OP pin (push-pull is fine; DS3231 doesn't clock-stretch); SDA is open-drain via an OP→N-FET pull-down with an IP reading back.  Optional INT#/SQW → a DUART change-detect IP for a 1 Hz/alarm IRQ.  (Chosen over the BQ3285, whose 146818-style multiplexed bus would have cost GLUE A17 + AS/DS strobes + 3 pins.)
* **GLUE ATF1508:** address decode (nRAM_[1-4]_SEL, nROM_SELECT, VIDEO, CF incl CS0/CS1 + AS-gated IORD/IOWR, DUART, ~PORTS_SELECT), DTACK/wait-state generation for everything including PORTS, WRITE_LO/WRITE_HI byte write strobes, the boot ROM overlay + CONFIG register, autovector ~VPA, the PS/2 **keyboard** frame engine (RX one IRQ/byte, TX on the device clock — the keyboard stays here; the mouse is a second engine over in PORTS), ~PORTS_IRQ into the priority encoder at level 2, and a separate DEBUG-LED driver used for boot codes / video-ISR heartbeat / double-fault.  For the 0xC00000 direct-bus region GLUE emits just two timed strobes — ~IO_RD_EN (region & AS & R/~W) and ~IO_WR_EN (region & UDS & LDS & ~R/W) — and a 74155 fans them out by A19:18 into ~AUDIO_W plus spares, so further direct peripherals cost zero GLUE pins.  No ENGINE select — ENGINE self-decodes its MMIO from the bus it already masters (verify ENGINE fit).
  * **Fit finding (2026-07-28, cpld/glue/glue_rev2_ports.v):** the rev-2 GLUE *logic* fits comfortably — 107/128 LC, 56 FF, 306 PT — but **only with a fresh pinout**.  Refitting with the rev-1 hand-assigned `.pin` kept (`-preassign keep`, the flags the production target uses) fails to route no matter what: dropping `-strategy debug` and adding `Foldback_logic = on` change nothing.  So derive the rev-2 GLUE pinout from a `-preassign ignore` fit and freeze *that* before routing the board.  It lands nearly pin-full — **60/64 I/O + 4/4 dedicated inputs** — so GLUE has essentially no spare pins left in rev 2; anything more wants TQFP100 or a move into PORTS.
* **Serial I/O — XR68C681 DUART:** Channel A = console (115200 8N1; boot/panic output, no more bit-bang — pre-DUART failures are LED-blink only), Channel B = 2nd serial brought out to a TTL header (ESP32 / PPP / an RS232 level-shifter module) — deliberately not routed through USB.  C/T = 100 Hz systick (level 5) + a configurable timer ISR.  OP/IP pins host RTS/CTS flow control on both channels and the DS3231 I²C; allocation table in griffin.yml; spares remain.  (OP4/OP5 used to drive the paddle DUMP/~CLR pair — freed 2026-07-28, PORTS does it from one register bit.)
* Console UART header
* JTAG programming 1×6: TCK/TMS/TDI/TDO + 5 V + GND) 
* **Debug headers — LA access + "PCB design failed" backup:** two 2×15 0.1" headers laid out to **mirror the LogicAnalyzer level-shifter board's own 2×15 pinout** (24 channels + GND + 3V3 + 5V-reference + 2 external-trigger pins each), so the two on-hand 5 V-tolerant analyzers plug straight on and daisy-chain via their own 3-pin chain connectors into one synchronized 48-channel capture.  Exact channel-to-pin mapping, mating gender, and orientation come from the gusmanb KiCad files at schematic time — the wiki shows it only as a diagram (https://github.com/gusmanb/logicanalyzer/wiki/02---LogicAnalyzer-Hardware).  Channel plan (47 bus/control signals + SYSCLK = 48, an exact fit): **header 1 "what happened"** = D0–D15, ~AS, ~UDS, ~LDS, R/~W, ~DTACK, ~RESET, ~HALT (exactly 23); **header 2 "where"** = A1–A23 + SYSCLK (24).  5V-reference pins fed from the Griffin rail (analyzer's onboard jumper removed → external reference); 3V3 pins NC; trigger pins NC/test pads.  Roles: (1) *Observe:* whole-bus captures as above.  (2) *Backup bus-master:* every signal a master needs is on these same two headers — hold ~RESET+~HALT (68000 tri-states A/D/strobes; GLUE still does its normal decode, so the flash sees ordinary write cycles) - Bus-cycle mechanics for whoever masters: bit-banged/PIO read_word/write_word (honor ~DTACK or run conservatively slow) 
* **Video — VGA 640×480@60 1bpp** (NTSC/composite dropped) over shared SRAM via a pair of 7200 FIFOs.
  * VIDEO CPLD: HSYNC/VSYNC/timing, reads bytes from the 7200s, vsync IRQ (level 6), ENABLE *after* ENGINE.  Palette is in-band — the first word of each scanline carries {fg,bg} (R3G3B2 each), latched from the FIFO during hblank; no PALETTE register.  Bitfile stretch if it fits: 2bpp 320×240 with 4 palette entries (no board impact).
  * ENGINE CPLD: 16-bit DMA master (level 3).  SOURCE_PAGE = A[23:16] of the 64K-aligned framebuffer; streams VIDEO_WORDS_PER_LINE words/scanline in ENGINE_WORDS_PER_BURST bursts paced to HBLANK; ENABLE *before* VIDEO.  Per-line streaming lets the CPU flip SOURCE_PAGE between frames for double-buffering.  Must reset to DMA-disabled on system ~RESET (guaranteed, not power-on-luck) so the bus is quiet under external bus mastering.
* **Joysticks — 2× DE-9, Atari 2600-style, via 2× 74HCT245N (4 on hand):** one transceiver per byte lane (stick 1 → D[15:8], stick 2 → D[7:0]), DIR tied joystick→bus, both ~OE from ~JOY_OE (74155 read section, gated by GLUE ~IO_RD_EN) — no write logic, zero-wait DTACK, and the '245s are a sacrificial ESD boundary between the connectors and everything else.  One `move.w` reads both sticks; bits active-low (0 = switch closed).  U/D/L/R on DE-9 pins 1–4, fire on pin 6; also wire pins 5/9 (Sega Master System pad button 2 works, pads still manufactured), leaving spare bits per lane.  Pull-ups on every switch line, +5 V on pin 7 via polyfuse, GND pin 8.  Polled at 60 Hz in the vsync ISR — no IRQ, no CPLD state machine; registers defined in griffin.yml so the emulator can map them to keys before the board exists.
  * **Paddles — one port, 2 paddles.**  A 2600 paddle is a 1 MΩ pot from +5 V (pin 7) to pin 5/9 with fire on pins 3/4, so the connector wiring above already covers it.  Per paddle: ~10 nF cap to GND + 2N7002 drain FET on the pot line.  ~~2× 74HC590~~ — *superseded 2026-07-28:* the counters are **inside PORTS** now.  PORTS runs an 8-bit saturating upcounter per paddle at the full VGA line rate (still exactly the 2600's method: count while the RC ramp holds the pin below the input threshold, freeze at the crossing; t ≈ 0.69·RC, size C for ≈255-line ≈8.1 ms full scale) and the two FET gates come off one PADDLE_CONTROL.DUMP bit instead of the DUART OP4/OP5 pair.  CPU cost per frame is two byte reads and one byte write; still immune to DMA/IRQ jitter.  Expect software calibration (the input threshold varies part to part, and the pot parallels the pin-5/9 pull-up).  Paddle-vs-stick is a per-port software mode, as on the real 2600.
* **PORTS ATF1508 (new 2026-07-28) — the fourth CPLD, 0xFC0000:** PS/2 **mouse** frame engine (same design as GLUE's keyboard one, same register offsets so one firmware driver serves both), both **joystick** ports read as bytes, both **paddle** counters + the dump control, and the **audio FIFO pop** strobe with the 7202 ~HF IRQ latch.  It carries no bus timing of its own: GLUE hands it a cycle-qualified ~PORTS_SELECT and answers DTACK, exactly as for CF.  Its whole timebase is one net — **VIDEO's HSYNC tapped over to PORTS** (asynchronous, synchronized on arrival): paddles tick at the full 31.469 kHz line rate, and a single toggle FF halves it to 15.73 kS/s for the FIFO pop.  No prescaler, no programmable divider, anywhere.  Measured fit **111/128 LC, 80 FF, 41/64 I/O, 311 PT in PLCC84** — see pcbv2-ports-design.md.  This is what retires the 2× 74HCT245 and 2× 74HC590.
* **Audio - FIFO** (replaces the Rev 1 '373 latch): 2× 7202 FIFO + 2× 8-bit R2R DAC, stereo (L = D[15:8], R = D[7:0] straight to the FIFOs).  ~W = ~AUDIO_W from the 74155 write section (GLUE ~IO_WR_EN qualifies deliberate full-word writes — no A17 alias needed); **PORTS** generates ~R by dividing its HSYNC tap by two, and ~HF goes to PORTS, which latches it into ~PORTS_IRQ (level 2).  CPU fills the FIFO on that IRQ and acks with PORTS AUDIO_CONTROL; baseline ~15.7 kS/s; no mono packing.  (Was "VIDEO generates ~R"; changed 2026-07-28 when the pop moved into PORTS.)
* **Interrupts (autovector):** VIDEO 6, DUART 5, PS/2 keyboard 4, ENGINE 3, PORTS 2 (mouse + audio half-full, wire-ORed inside the CPLD).
* **Address map:** RAM 0x000000–0x3FFFFF; **ROM window 0x800000–0xBFFFFF** (decode = A23 & ~A22, two literals; the 128 KB image mirrors 32×); all peripherals under A23 & A22: **direct-bus region 0xC00000–0xCFFFFF** split /4 by A19:18 — AUDIO 0xC00000, 0xC40000/0xC80000 reserved (were JOYSTICK/PADDLE before they became PORTS registers), 0xCC0000 reserved (ENGINE COPY-FIFO candidate) — strobed by the 74155 from GLUE's timed ~IO_RD_EN/~IO_WR_EN, data never passing through a CPLD; ENGINE 0xD00000 (self-decoded), VIDEO 0xE00000, GLUE 0xF00000, CF 0xF40000, DUART 0xF80000, **PORTS 0xFC0000**.  Registers and constants in griffin.yml.

## Board changes

*(KiCad watch-list — footprints, pinouts, nets, placement, SI.  Function lives in Rev 2 components above; link, don't restate.)*

### Footprints & sockets
- [ ] ROM: 2× W27C512 DIP-28 in sockets (one per byte lane, 128 KB ×16, as Rev 1); /OE←nR_W, /CE←nROM_SELECT; images via the bench programmer
- [ ] RAM: 8× DIP-32 sockets, 4 banks × 2 of AS6C4008 (populate contiguously from bank 1, as Rev 1); /OE←nR_W, WRITE_LO/WRITE_HI byte strobes + nRAM_[1-4]_SEL from GLUE
- [ ] Audio: 2× 7202 FIFO + 2× R2R DAC on the board (replaces the Rev 1 '373 + write-only ~AUDIO_LE); ~W from GLUE, **~R and the ~HF IRQ from PORTS** (was VIDEO's line clock — changed 2026-07-28)
- [ ] Video FIFOs: 2× 7200 on the board natively (retires the piggyback bodge of video-fifo-wiring.md); termination in SI section below
- [ ] VGA: DE-15 connector; remove the composite/NTSC jack and any NTSC clock provisions — Rev 2 is VGA-only
- [ ] DUART: XR68C681 DIP-40 native (retires the DIP-carrier bodge); A1-A4→RS1-RS4, D0-7 + R/~W direct, ~RESET from ~RESET net, ~IACK tied high (autovectors), 3.6864 MHz crystal on X1/X2 (clearance if ZIF)
- [ ] Console: DUART Ch A TTL header (FTDI-cable pinout); Ch B TTL header (RS232 level-shifter module optional)
- [ ] RTC: DS3231 + coin-cell holder; SCL=OP2, SDA via OP3→2N7002 (drain on SDA) + IP2 readback
- [ ] PS/2: fix footprint + pin mapping — **two ports now**: keyboard CLK/DATA to GLUE, mouse CLK/DATA to PORTS (changed 2026-07-28)
- [ ] PORTS: ATF1508AS PLCC84 socket + JTAG chain; nets ~PORTS_SELECT / ~PORTS_IRQ / A4-A1 / ~LDS / R~W / D7-D0 to GLUE and the bus, **HSYNC tapped from VIDEO** to PORTS, ~R and ~HF to the 7202 pair, mouse PS/2, both DE-9s, PADDLE_DUMP to the FET gates
- [ ] Peripheral decode: 74155 DIP-16 (bin part, 3 on hand) as dual 2-to-4 for the 0xC00000 direct-bus region — A/B←A19:18; section 1 (1C→+5V, 1G←~IO_RD_EN): 1Y1/1Y2 now spare (were ~JOY_OE/~PADDLE_OE before PORTS), 1Y3=~COPY_RD reserved, 1Y0 spare; section 2 (2C→GND, 2G←~IO_WR_EN): 2Y0=~AUDIO_W, 2Y3=~COPY_WR reserved, 2Y1/2Y2 spare.  ~IO_RD_EN/~IO_WR_EN are the only 2 GLUE pins for the whole region; ENGINE self-decodes MMIO (no nENGINE_SELECT net); CF CS1 stays routed from GLUE
- [ ] Joysticks: 2× DE-9 male PCB-mount, switch lines **straight into PORTS** — no '245s (superseded 2026-07-28); bussed pull-up networks, polyfuse on pin-7 +5V, optional series R + clamps between connector and the CPLD
- [ ] Paddles (port 1 only): 2× {~10 nF film cap to GND + 2N7002 drain, gate←PADDLE_DUMP from PORTS} on DE-9 pins 5/9, pin 5/9 also to PORTS as the count-enable senses — no '590s (superseded 2026-07-28); all DNP-able, a sticks-only build omits the caps/FETs
- [ ] everything THT / socketed where practical — Rev 2 stays bodgeable by intent
- [ ] Headphone-jack pads / RCA retainer feet — partial holes?
- [ ] JTAG programming header 1×6 (TCK/TMS/TDI/TDO/5V/GND) — the primary CPLD programming path (external dongle, as Rev 1)
- [ ] Debug headers: 2× 2×15 THT mirroring the gusmanb level-shifter pinout (pull channel map + gender/orientation from the project KiCad; leave clearance for two analyzers side by side or ribbon out); header 1 = D0–D15 + ~AS/~UDS/~LDS/R/~W/~DTACK + ~RESET/~HALT (23 — spare-channel candidates: audio line clock or paddle DUMP), header 2 = A1–A23 + SYSCLK (via a buffered/series-R tap — do not stub the raw clock net); 5V-ref pins from the rail, 3V3 NC, triggers NC/pads

### Power & decoupling
- [ ] USB-C power input with inline switch + two 5.1k CC pull-downs, carried over from Rev 1 (draws 0.84 A on a plain 5 V sink — no PD needed)
- [ ] decoupling cap on every +5V/GND pair, especially the CPLDs

### Pull-ups
- [ ] 4.7K on HALT; any CPU lines that may float or lead
- [ ] ~AS, ~UDS, ~LDS, R/~W (and ~DTACK if ever tri-stated) — strobes must idle deasserted while the CPU is tri-stated (~RESET+~HALT bus-master mode via the debug headers), else GLUE sees phantom cycles; also cleans up LA captures
- [ ] PS/2 CLK & DATA
- [ ] joystick switch lines (bussed resistor networks to +5 V) — except DE-9 pins 5/9 on the paddle port: weak 1 MΩ discretes there, since the paddle pot parallels the pull-up (still fine for reading SMS button 2, just slow edges)
- [ ] I²C SCL/SDA to 5 V
- [ ] JTAG lines
- [ ] any inter-IC signal that could stall or float

### Signal integrity & analog
- [ ] SYSCLK into a GCLK on every CPLD (especially GLUE)
- [ ] source termination (33–100 Ω series at CPLD output) on the FIFO control lines — /RE_EVEN, /RE_ODD, /W, q8_toggle (Rev 1: unterminated bodge wires rang and doubled reads → image creep; 10 pF-to-GND was the stopgap)
- [ ] 7200s on the bus may need transceivers for capacitance; inline R + cap-to-GND per the jumper-to-solderless experiment (the 2 spare 74HCT245N would serve)
- [ ] redesign the VGA analog (R-2R ladder + sync) to be robust; place the audio R2R/op-amp near the jack

### Test & debug access
- [ ] bring every inter-IC signal (GND, +5V, D, A, WRITE_LO/HI, RAM/ROM/IO/VIDEO/ENGINE selects, nVPA) to a 2×N test header with the signal silk-screened per pin
- [ ] female header sized for Dr. Guzman's analyzer (try two ganged); lots of ground test-point holes
- [ ] spare GLUE pins to a debug header if any remain
- [ ] wire all CPLDs (GLUE/VIDEO/ENGINE/PORTS) into the JTAG chain
- [x] separate DEBUG LED + NPN driver (2N3904 / SOT-23 MMBT3904; base via 1–10 K to DEBUG_OUT)

### Carryover bug fixes
- [ ] fold every entry of the Rev 1 bodge record (above) into the schematic natively
- [ ] CF symbol is junk — redo it (weird pin numbers)
- [ ] CF IOWR must be gated by AS in GLUE — R/~W alone leaves AS gone at IOWR rise → junk data
- [ ] CF to 16 bits
- [ ] CF DMACK to +5; CS0/CS1 are swapped (fixed in Verilog for now)
- [ ] flip the FTDI (currently 180° / upside-down on the 90° header)
- [x] A18→GLUE (was A6); GLUE VPA→CPU (was ENGINE_IACK)

### Process & layout
- [ ] HDL first: bring all CPLD bitfiles (GLUE/VIDEO/ENGINE) to a fit with the fitter free to minimize macrocells, then freeze the pinout before routing
- [ ] hub-and-spoke: bus across from the CPU, peripherals above/below with vertical taps; connectors to the rear
- [ ] open: more inter-CPLD signals? (decide during HDL)
- [ ] BOM output with an "I already have these" filter

## Shrinkwrapped VCFWest package

Couple of completed machines:

* Rev 2 - if ENGINE can do generalized DMA chain, do that
  * accelerated memmove and scroll and CF card (firmware routines, Linux)
* 3D printed case
* VGA monitors, try to get CRTs - Jim?

Posters

* Block architecture
* Interesting features

Demos

* Linux - can it support a windowing system?
  * Move to erofs for root and XIP?
* BASIC
* Zaxxon? Or other interesting game
* Movie player with audio
* Jukebox with album art

# Rev 3

## SW Investigation Plan

???

## Possible new hardware features (July 24 2026)

Video through a CLUT RAM?  For 640 pixels wide need a ~15-25ns part, don't have one in my kit

## Strategy

* Use Rev 2 strategy plus any revelations gathered along the way

## Rev 3 components

*(Parts and their function — the Rev 3 spec.  Footprints, nets, placement, and layout gotchas live in Board changes, not here.)*

* **CPU:** 68010 primary (68000 is a drop-in), 14 MHz SYSCLK.
* **Clocking:** 14 MHz system oscillator + a separate 25.175 MHz pixel oscillator for VGA.  SYSCLK feeds a GCLK on each CPLD.
* **RAM:** 16-bit-wide async SRAM, 8 MB.  One AS6C6416 (4M×16 = 8 MB, 55 ns) at 0x000000–0x7FFFFF; GLUE decodes a single chip select (A23==0), byte lanes from UDS/LDS — no SDRAM/PSRAM, no memory controller.  (Was 2 chips / 12 MB; 0x800000–0xBFFFFF is now the flash window — see ROM.)
* **ROM:** 1–2× M29F160FB (16 Mbit 5 V NOR, 1M×16, 55 ns, TSOP48), soldered — replaces the 2× SST39SF040 plan to make room for a Linux kernel + erofs root in flash.  Still in production (Alliance Memory second source; DigiKey/Mouser/TME stock).  FB (bottom boot) preferred: the 16/8/8 KB boot sectors sit at the vector/bootloader end while the erofs image lives in the uniform 64 KB sectors above (FT differs only in sector order).  ×16 mode: BYTE# tied high, flash A0–A19 ← CPU A1–A20, D0–D15 direct — one chip puts 2 MB on the full 16-bit bus (vs 1 MB from the SST pair); an optional second chip selected by A21 makes 4 MB.  Flash window moves 0xC00000 → 0x800000–0xBFFFFF (cheaper decode).  55 ns may allow zero-wait ROM at 14 MHz.  Initial flash and recovery via the on-board RP2350B bridge's UF2 drive (see below), which bus-masters JEDEC cycles into the flash window with the CPU held off.  (Offline fallback: XGecu T48 + ADP_F48_EX-1 TSOP48 adapter)  The CPU can also self-reflash in-circuit via the GLUE ~ROM_WE strobe gated by CONFIG.FLASH_WE_EN (default write-protected), JEDEC 0x555/0x2AA unlock sequences as plain word-writes, flasher runs from RAM (not read-while-write).  RESET# to the ~RESET net.
* **CF:** 16-bit True IDE PIO; IOWR gated by AS (needs a glue.v change: R/~W + AS → IOWR).
* **RTC:** DS3231 (TCXO) on I²C via the DUART's spare OP/IP pins — off the bus, zero GLUE logic (no A17, no chip-select/strobes/wait-states), 5 V so no level translation.  SCL = an OP pin (push-pull is fine; DS3231 doesn't clock-stretch); SDA is open-drain via an OP→N-FET pull-down with an IP reading back.  Optional INT#/SQW → a DUART change-detect IP for a 1 Hz/alarm IRQ.  (Chosen over the BQ3285, whose 146818-style multiplexed bus would have cost GLUE A17 + AS/DS strobes + 3 pins.)
* **GLUE ATF1508:** address decode, ~R/W + write-strobe generation, autovector ~VPA, the PS/2 keyboard frame engine (RX one IRQ/byte, TX on the device clock; the mouse is in PORTS from rev 2 on), and a separate DEBUG-LED driver used for boot codes / video-ISR heartbeat / double-fault.  Decodes ~AUDIO_SEL for the AUDIO CPLD? Drives  ~ROM_WE flash strobe.  ~BOOTSTRAP input from the RP2350B bridge or test header: while asserted, ~ROM_WE follows flash-window writes regardless of CONFIG.FLASH_WE_EN, and any AS-stuck watchdog is relaxed so slow external bus cycles aren't killed.
* **Serial I/O — XR68C681 DUART:** Channel A = console (115200 8N1; boot/panic output, no more bit-bang — pre-DUART failures are LED-blink only), Channel B = 2nd serial brought out to a TTL header (ESP32 / PPP / an RS232 level-shifter module) — deliberately not routed through USB.  C/T = 100 Hz systick (level 5) + a configurable timer ISR.  OP/IP pins host RTS/CTS flow control on both channels and the DS3231 I²C; spares remain.
* **RP2350B bus-master (USB-C #1, on-board):**   47 of 48 GPIOs: A1–A23 (23 — A23:22 must be driven to reach the flash window at 0x800000 and the rest of the map), D0–D15 (16), ~AS/~UDS/~LDS/R/~W (4), ~DTACK (1), ~RESET/~HALT/~BOOTSTRAP (3, open-drain on the existing nets).  USB rides the dedicated DP/DM pins (zero GPIO), and no VBUS-sense pin is needed: its USB-C is the board's power input, so VBUS is present whenever the board is on — dumb charger and host-peripheral both work — which also guarantees the powered-5V-tolerance precondition; no level translators.  USB device = **UF2 virtual-FAT drive** (GRIFFIN volume with INFO_UF2.TXT and a CURRENT.BIN readback file; host writes rom.uf2 → assert ~RESET+~HALT+~BOOTSTRAP, bus-master JEDEC erase/program/verify into the flash window, release reset on eject) plus a CDC command channel for scripted flashing and peek/poke — the bring-up tool: read/write RAM and peripheral registers with no working CPU or ROM.  Self-programming via the RP2350 ROM's BOOTSEL USB bootloader on its own port; SWD test points as backup.  Safety: GPIOs default Hi-Z at reset; watchdog so a crash can't stay driving the bus; external resistors, not internal pull-downs (erratum RP2350-E9).  If the one spare pin ever isn't enough, the ~BOOTSTRAP-gated 74HC595 pair on the upper address lines frees ~13.
* **FT2232H bridge (USB-C #2, data-only):** Channel A (MPSSE) = JTAG to the CPLD chain; B = console ↔ DUART Ch A.  Self-powered from the board 5 V rail (shared 3.3 V LDO); its connector does **not** feed the rail — VBUS goes only to an enumerate-when-cabled sense divider, so there is no back-power path with two hosts attached.  3.3 V and not 5 V-tolerant: lines driving *into* it (CPLD TDO, DUART TxD) get level translation; 3.3 V→5 V lines drive direct.  12 MHz crystal + optional 93LC56 EEPROM.
* **Debug headers — LA access + "PCB design failed" backup:** two 2×15 0.1" headers laid out to **mirror the LogicAnalyzer level-shifter board's own 2×15 pinout** (24 channels + GND + 3V3 + 5V-reference + 2 external-trigger pins each), so the two on-hand 5 V-tolerant analyzers plug straight on and daisy-chain via their own 3-pin chain connectors into one synchronized 48-channel capture.  Exact channel-to-pin mapping, mating gender, and orientation come from the gusmanb KiCad files at schematic time — the wiki shows it only as a diagram (https://github.com/gusmanb/logicanalyzer/wiki/02---LogicAnalyzer-Hardware).  Channel plan (47 bus/control signals + SYSCLK = 48, an exact fit): **header 1 "what happened"** = D0–D15, ~AS, ~UDS, ~LDS, R/~W, ~DTACK, ~RESET, ~HALT, ~BOOTSTRAP (exactly 24); **header 2 "where"** = A1–A23 + SYSCLK (24).  5V-reference pins fed from the Griffin rail (analyzer's onboard jumper removed → external reference); 3V3 pins NC; trigger pins NC/test pads.  Roles: (1) *Observe:* whole-bus captures as above.  (2) *Backup bus-master:* every signal a master needs is on these same two headers — hold ~RESET+~HALT (68000 tri-states A/D/strobes; GLUE still does its normal decode, so the flash sees ordinary write cycles) — for when the on-board RP2350B is the thing being debugged or its firmware is bricked mid-development.  Bus-cycle mechanics for whoever masters: bit-banged/PIO read_word/write_word (honor ~DTACK or run conservatively slow) + JEDEC unlock/erase/program/verify; programming is flash-limited (~10 µs/word ⇒ ~20–30 s per 2 MB chip).  (3) *Backup CPLD programming* on a small third strip (1×6: TCK/TMS/TDI/TDO + 5 V + GND) — the rev-1-style external JTAG dongle path, live even if the FT2232H path is broken; series resistors between the FT2232H and the JTAG lines let the strip cleanly overdrive an idle FTDI.
* **Video — VGA 640×480@60 1bpp** (NTSC/composite dropped) over shared SRAM via a pair of 7200 FIFOs.
  * VIDEO CPLD: HSYNC/VSYNC/timing, reads bytes from the 7200s, vsync IRQ (level 6), ENABLE *after* ENGINE.  Palette is in-band — the first word of each scanline carries {fg,bg} (R3G3B2 each), latched from the FIFO during hblank; no PALETTE register.  Bitfile stretch if it fits: 2bpp 320×240 with 4 palette entries (no board impact).
  * ENGINE CPLD: 16-bit DMA master (level 3).  SOURCE_PAGE = A[23:16] of the 64K-aligned framebuffer; streams VIDEO_WORDS_PER_LINE words/scanline in ENGINE_WORDS_PER_BURST bursts paced to HBLANK; ENABLE *before* VIDEO.  Per-line streaming lets the CPU flip SOURCE_PAGE between frames for double-buffering.  Must reset to DMA-disabled on system ~RESET (guaranteed, not power-on-luck) so the bus is quiet under external bus mastering.
* ~~**Audio — dedicated ATF1504AS (PLCC44) at 0xFC0000**~~ — **superseded 2026-07-28.**  The idea was a whole CPLD for the audio path: 2× 7202 + 2× R2R DAC with the CPLD generating ~W, ~R from a 12-bit SYSCLK divider, and ~HF → its own IRQ, self-clocked so no VIDEO line-rate signal needed.  Measured dead: the programmable divider is what costs — it is ~30 FF on its own, and every pairing that included it overflowed (PORTS + audio divider = 151 FF, rejected by the 1508 fitter before placement).  Dividing VIDEO's HSYNC tap by two costs **one flip-flop** and lands on the 15.73 kS/s we wanted anyway, so rev 2 does that inside PORTS and rev 3 should too.  0xFC0000 is PORTS.  See pcbv2-ports-design.md and pcbv2-audio-design.md.
* **Interrupts (autovector):** VIDEO 6, DUART 5, PS/2 keyboard 4, ENGINE 3, PORTS 2 (mouse + audio half-full).

## Board changes

*(KiCad watch-list — footprints, pinouts, nets, placement, SI.  Function lives in Rev 3 components above; link, don't restate.)*

### Footprints & sockets
- [ ] ROM: 1–2× M29F160FB TSOP48, **soldered** (image-zero via the RP2350B bridge; bench T48 + ADP_F48_EX-1 as fallback); route ~ROM_WE (new net), /OE←nR_W, /CE←nROM_SELECT (2nd chip select by A21), BYTE#→+5V, RESET#→~RESET net
- [ ] RAM: 1× AS6C6416 TSOP-II; /UB←nUDS, /LB←nLDS, /OE←nR_W
- [ ] Audio: no dedicated CPLD (superseded 2026-07-28) — ~W from GLUE's direct-bus strobe, ~R and the ~HF IRQ from PORTS
- [ ] DUART: XR68C681 DIP-40; A1-A4→RS1-RS4, D0-7 + R/~W direct, ~RESET from ~RESET net, ~IACK tied high (autovectors), 3.6864 MHz crystal on X1/X2 (clearance if ZIF)
- [ ] RP2350B: QFN-80 + QSPI flash + 12 MHz crystal; own USB-C (#1 = board power input, rear); BOOTSEL button + SWD test points; route A1–A23, D0–D15, ~AS/~UDS/~LDS/R/~W, ~DTACK, ~RESET/~HALT/~BOOTSTRAP (47 of 48 GPIOs — one spare)
- [ ] FT2232H: USB-C #2 (data-only, rear); 12 MHz crystal, optional 93LC56 EEPROM; port A JTAG → CPLD chain through series isolation resistors (JTAG backup lives on the debug header, no separate 2×5), port B ↔ DUART Ch A
- [ ] DUART Ch B TTL header (RS232 level-shifter module optional)
- [ ] RTC: DS3231 + coin-cell holder; SCL=OP2, SDA via OP3→2N7002 (drain on SDA) + IP2 readback
- [ ] PS/2: fix footprint + pin mapping — two ports, keyboard to GLUE and mouse to PORTS (changed 2026-07-28)
- [ ] decide SMT vs THT for CF, RAM, DUART, and the CPLDs
- [ ] Headphone-jack pads / RCA retainer feet — partial holes?
- [ ] Debug headers: 2× 2×15 THT mirroring the gusmanb level-shifter pinout (pull channel map + gender/orientation from the project KiCad; leave clearance for two analyzers side by side or ribbon out); header 1 = D0–D15 + ~AS/~UDS/~LDS/R/~W/~DTACK + ~RESET/~HALT/~BOOTSTRAP, header 2 = A1–A23 + SYSCLK (via a buffered/series-R tap — do not stub the raw clock net); 5V-ref pins from the rail, 3V3 NC, triggers NC/pads; plus 1×6 JTAG backup strip (TCK/TMS/TDI/TDO/5V/GND); keep stubs short; route ~BOOTSTRAP to GLUE (and the RP2350B bridge)

### Power & decoupling
- [ ] USB-C #1 (RP2350B) with two CC sink resistors is the board power input (no PD — Rev 1 draws 0.84 A on a plain 5 V sink; power-only charger and host-peripheral both fine, hosts advertise ≥1.5 A on C); USB-C #2 (FT2232H) is data-only — VBUS to a sense divider, never the rail, so no back-power path
- [ ] decoupling cap on every +5V/GND pair, especially the CPLDs
- [ ] 3.3 V LDO for the RP2350B (IOVDD; core is internally regulated)

### Pull-ups
- [ ] 4.7K on HALT; any CPU lines that may float or lead
- [ ] ~AS, ~UDS, ~LDS, R/~W (and ~DTACK if ever tri-stated) — strobes must idle deasserted while the CPU is tri-stated (~RESET+~HALT bus-master mode), else GLUE sees phantom cycles; also cleans up LA captures
- [ ] PS/2 CLK & DATA
- [ ] I²C SCL/SDA to 5 V
- [ ] JTAG lines
- [ ] any inter-IC signal that could stall or float

### Signal integrity & analog
- [ ] SYSCLK into a GCLK on every CPLD (especially GLUE)
- [ ] source termination (33–100 Ω series at CPLD output) on the FIFO control lines — /RE_EVEN, /RE_ODD, /W, q8_toggle (Rev 1: unterminated bodge wires rang and doubled reads → image creep; 10 pF-to-GND was the stopgap)
- [ ] 7200s on the bus may need transceivers for capacitance; inline R + cap-to-GND per the jumper-to-solderless experiment
- [ ] level translation on the 5V→3.3V lines into the FT2232H (CPLD TDO, DUART TxD); the RP2350B needs none (5 V-tolerant while powered, and always powered with the board)
- [ ] redesign the VGA analog (R-2R ladder + sync) to be robust; place the audio R2R/op-amp near the jack

### Test & debug access
- [ ] bring every inter-IC signal (GND, +5V, D, A, WRITE_LO/HI, IO/VIDEO/ENGINE/PORTS select, nVPA) to a 2×N test header with the signal silk-screened per pin
- [ ] female header sized for Dr. Guzman's analyzer (try two ganged); lots of ground test-point holes
- [ ] spare GLUE pins to a debug header if any remain
- [ ] wire all CPLDs (GLUE/VIDEO/ENGINE/PORTS) into the JTAG chain — also the boundary-scan-flash path
- [x] separate DEBUG LED + NPN driver (2N3904 / SOT-23 MMBT3904; base via 1–10 K to DEBUG_OUT)

### Carryover bug fixes
- [ ] CF symbol is junk — redo it (weird pin numbers)
- [ ] CF IOWR must be gated by AS in GLUE — R/~W alone leaves AS gone at IOWR rise → junk data
- [ ] CF to 16 bits
- [ ] CF DMACK to +5; CS0/CS1 are swapped (fixed in Verilog for now)
- [ ] flip the FTDI (currently 180° / upside-down on the 90° header)
- [x] A18→GLUE (was A6); GLUE VPA→CPU (was ENGINE_IACK)

### Process & layout
- [ ] HDL first: bring all CPLD bitfiles (GLUE/VIDEO/ENGINE/PORTS) to a fit with the fitter free to minimize macrocells, then freeze the pinout before routing
- [ ] hub-and-spoke: bus across from the CPU, peripherals above/below with vertical taps; RP2350B + FT2232H with both USB-Cs to the rear
- [ ] open: more inter-CPLD signals? (decide during HDL)
- [ ] BOM output with an "I already have these" filter

## Possible Linux target

m68k NOMMU build - may have bitrotted versus Coldfire

2MB not likely to be capable of much, 4MB is better, 12MB would be best (use all address space available other than peripherals)

https://github.com/AcceleratedLinux/accelerated-linux

https://github.com/fifteenhex/m68kjunk

# Rev 4

68030 + 68882 + >=64MB DRAM + USB (by RP2350?) + Ethernet + at least 800x600x8? - make a proper Linux workstation

3.3V, FPGAs
