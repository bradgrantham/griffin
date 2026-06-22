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

* Console
  * What to do about PS/2?  Want some kind of raw SDL/GLFW-like keycode operation for graphical apps.
    * Some kind of "switch to raw mode" call; open "/dev/keyboard" and that becomes a raw keycode reader?
    * Open "/dev/fb" and text is discarded?
* SW improvements
  * graphics routines, take "blit" out of splash.cpp
  * factor out font - should be selectable by enum
  
* Booter & apps
  * Need trap interface to ROM calls
    * get_time, open/close/read/write/etc, sbrk?
    * read(0), write(0) for console
    * open("/dev/ttyS0") for DUART port 2 (not currently connected - how to test?  bodge another 6-pin?)
  * to enable loadable apps : configure linker.ld, load at 0x1000, crt0.s that just sets up program and rts when done?, syscalls.c that pulls trap
  * Load file into memory, jump to 0x1000
  * Have a shell in the ROM
  * Image viewer app
  * BASIC (finish up your basic.cpp)
* Get Linux NOMMU proof of concept or another OS running, at the very least a toolchain that allows you to run apps from CF card; expect to have 12MB on Rev 2
  * buildroot
    * Need kernel config for: serial, PPP, block devices, CF card, ext4, console with PS/2 and bitmap display
      * bonus: fbdev
    * need serial driver for xr68C681
    * need later a console driver hooking together PS/2 and framebuffer
    * need 68010 config, seems like Claude can get on top of that

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
* **RAM:** 16-bit-wide async SRAM, ~12 MB usable.  Two AS6C6416 (4M×16 = 8 MB each, 55 ns) populate 16 MB; only 0x000000–0xBFFFFF (12 MB) is addressable (top 4 MB is peripherals).  GLUE decodes just 2 chip selects (chip0 = A23==0, chip1 = A23 & ~A22) instead of 4 banks, byte lanes from UDS/LDS — no SDRAM/PSRAM, no memory controller.
* **ROM:** 2× SST39SF040 (1 MB, 16-bit, 5V, 70 ns), socketed.  In-circuit reflashable via a GLUE ROM-region write strobe (~ROM_WE) gated by CONFIG.FLASH_WE_EN (default write-protected); word-writes only, flasher runs from RAM (not read-while-write).  Image-zero is bench-programmed; stretch goal is first-program / OTA over JTAG by driving the CPLD-chain boundary scan (CPU in reset, address from ENGINE, data from VIDEO/GLUE, ~CS+~WE from GLUE).
* **CF:** 16-bit True IDE PIO; IOWR gated by AS (needs a glue.v change: R/~W + AS → IOWR).
* **RTC:** DS3231 (TCXO) on I²C via the DUART's spare OP/IP pins — off the bus, zero GLUE logic (no A17, no chip-select/strobes/wait-states), 5 V so no level translation.  SCL = an OP pin (push-pull is fine; DS3231 doesn't clock-stretch); SDA is open-drain via an OP→N-FET pull-down with an IP reading back.  Optional INT#/SQW → a DUART change-detect IP for a 1 Hz/alarm IRQ.  (Chosen over the BQ3285, whose 146818-style multiplexed bus would have cost GLUE A17 + AS/DS strobes + 3 pins.)
* **GLUE ATF1508:** address decode, ~R/W + write-strobe generation, autovector ~VPA, the PS/2 keyboard frame engine (RX one IRQ/byte, TX on the device clock; no mouse port), and a separate DEBUG-LED driver used for boot codes / video-ISR heartbeat / double-fault.  Decodes ~AUDIO_SEL for the AUDIO CPLD and drives the new ~ROM_WE flash strobe.  Net Rev 2 effect: GLUE is *smaller* than Rev 1 (RAM 4 selects→2, RTC off-chip, only ~ROM_WE added).
* **Serial I/O — XR68C681 DUART:** Channel A = console (115200 8N1; boot/panic output, no more bit-bang — pre-DUART failures are LED-blink only), Channel B = 2nd serial for ESP32 / PPP / network at the highest sustainable baud.  C/T = 100 Hz systick (level 5) + a configurable timer ISR.  OP/IP pins host RTS/CTS flow control on both channels and the DS3231 I²C; spares remain.
* **FT4232H bridge (USB-C):** one chip, 4 independent ports over one cable — A (MPSSE) = JTAG to the CPLD chain (and the stretch-goal flash-over-JTAG); B = console ↔ DUART Ch A; C = 2nd UART ↔ DUART Ch B; D = host board control (assert CPU ~RESET/~HALT and the flash-bootstrap enable for hands-off reset/reflash).  It is 3.3 V and not 5 V-tolerant, so lines driving *into* it (CPLD TDO, DUART TxD, any RESET/HALT readback) go through 74LVC245s; 3.3 V→5 V lines drive direct.
* **Video — VGA 640×480@60 1bpp** (NTSC/composite dropped) over shared SRAM via a pair of 7200 FIFOs.
  * VIDEO CPLD: HSYNC/VSYNC/timing, reads bytes from the 7200s, vsync IRQ (level 6), ENABLE *after* ENGINE.  Palette is in-band — the first word of each scanline carries {fg,bg} (R3G3B2 each), latched from the FIFO during hblank; no PALETTE register.  Bitfile stretch if it fits: 2bpp 320×240 with 4 palette entries (no board impact).
  * ENGINE CPLD: 16-bit DMA master (level 3).  SOURCE_PAGE = A[23:16] of the 64K-aligned framebuffer; streams VIDEO_WORDS_PER_LINE words/scanline in ENGINE_WORDS_PER_BURST bursts paced to HBLANK; ENABLE *before* VIDEO.  Per-line streaming lets the CPU flip SOURCE_PAGE between frames for double-buffering.
* **Audio — dedicated ATF1504AS (PLCC44) at 0xFC0000** (replaces the Rev 1 '373 latch): 2× 7202 FIFO + 2× 8-bit R2R DAC, stereo (L = D[15:8], R = D[7:0] straight to the FIFOs).  The CPLD generates ~W (full-word writes to the A17-high alias), ~R (12-bit SYSCLK divider on a GCLK), and ~HF → nAUDIO_IRQ (level 2).  CPU fills the FIFO on the HF IRQ; baseline ~15.7 kS/s; no mono packing.  Self-clocked, so no VIDEO line-rate signal.  (Needs new audio.v + YAML registers.)
* **Interrupts (autovector):** VIDEO 6, DUART 5, PS/2 4, ENGINE 3, AUDIO 2.

## Board changes

*(KiCad watch-list — footprints, pinouts, nets, placement, SI.  Function lives in Rev 2 components above; link, don't restate.)*

### Footprints & sockets
- [ ] ROM: 2× SST39SF040 PLCC **sockets** (ZIF clearance — decoupling is currently too close); route ~ROM_WE (new net), /OE←nR_W, /CE←nROM_SELECT
- [ ] RAM: 2× AS6C6416 TSOP-II; /UB←nUDS, /LB←nLDS, /OE←nR_W
- [ ] Audio: ATF1504 PLCC44; route ~AUDIO_SEL (replaces write-only ~AUDIO_LE)
- [ ] DUART: XR68C681 DIP-40; A1-A4→RS1-RS4, D0-7 + R/~W direct, ~RESET from ~RESET net, ~IACK tied high (autovectors), 3.6864 MHz crystal on X1/X2 (clearance if ZIF)
- [ ] FT4232H: SMT footprint rotated so USB-C faces the rear; 12 MHz crystal, 3.3 V LDO, optional 93LC56 EEPROM; 2×5 JTAG header in parallel for an external programmer / isolation
- [ ] RTC: DS3231 + coin-cell holder; SCL=OP2, SDA via OP3→2N7002 (drain on SDA) + IP2 readback
- [ ] PS/2: fix footprint + pin mapping (single keyboard port)
- [ ] decide SMT vs THT for CF, RAM, DUART, and the CPLDs
- [ ] Headphone-jack pads / RCA retainer feet — partial holes?

### Power & decoupling
- [ ] USB-C with two CC sink resistors (no PD — Rev 1 draws 0.84 A on a plain 5 V sink)
- [ ] decoupling cap on every +5V/GND pair, especially the CPLDs
- [ ] 3.3 V LDO for the FT4232H

### Pull-ups
- [ ] 4.7K on HALT; any CPU lines that may float or lead
- [ ] PS/2 CLK & DATA
- [ ] I²C SCL/SDA to 5 V
- [ ] JTAG lines
- [ ] any inter-IC signal that could stall or float

### Signal integrity & analog
- [ ] SYSCLK into a GCLK on every CPLD (especially GLUE)
- [ ] source termination (33–100 Ω series at CPLD output) on the FIFO control lines — /RE_EVEN, /RE_ODD, /W, q8_toggle (Rev 1: unterminated bodge wires rang and doubled reads → image creep; 10 pF-to-GND was the stopgap)
- [ ] 7200s on the bus may need transceivers for capacitance; inline R + cap-to-GND per the jumper-to-solderless experiment
- [ ] 74LVC245 on the 5V→3.3V lines into the FT4232H (TDO, DUART TxD, RESET/HALT readback)
- [ ] redesign the VGA analog (R-2R ladder + sync) to be robust; place the audio R2R/op-amp near the jack

### Test & debug access
- [ ] bring every inter-IC signal (GND, +5V, D, A, WRITE_LO/HI, IO/VIDEO/ENGINE/AUDIO select, nVPA) to a 2×N test header with the signal silk-screened per pin
- [ ] female header sized for Dr. Guzman's analyzer (try two ganged); lots of ground test-point holes
- [ ] spare GLUE pins to a debug header if any remain
- [ ] wire all CPLDs (GLUE/VIDEO/ENGINE/AUDIO) into the JTAG chain — also the boundary-scan-flash path
- [x] separate DEBUG LED + NPN driver (2N3904 / SOT-23 MMBT3904; base via 1–10 K to DEBUG_OUT)

### Carryover bug fixes
- [ ] CF symbol is junk — redo it (weird pin numbers)
- [ ] CF IOWR must be gated by AS in GLUE — R/~W alone leaves AS gone at IOWR rise → junk data
- [ ] CF to 16 bits
- [ ] CF DMACK to +5; CS0/CS1 are swapped (fixed in Verilog for now)
- [ ] flip the FTDI (currently 180° / upside-down on the 90° header)
- [x] A18→GLUE (was A6); GLUE VPA→CPU (was ENGINE_IACK)

### Process & layout
- [ ] HDL first: bring all CPLD bitfiles (GLUE/VIDEO/ENGINE/AUDIO) to a fit with the fitter free to minimize macrocells, then freeze the pinout before routing
- [ ] hub-and-spoke: bus across from the CPU, peripherals above/below with vertical taps; FT4232H/USB-C to the rear
- [ ] open: more inter-CPLD signals? (decide during HDL)
- [ ] BOM output with an "I already have these" filter

## Possible Linux target

m68k NOMMU build - may have bitrotted versus Coldfire

2MB not likely to be capable of much, 4MB is better, 12MB would be best (use all address space available other than peripherals)

https://github.com/AcceleratedLinux/accelerated-linux

https://github.com/fifteenhex/m68kjunk

# Rev 3

68030 + 68882 + >=32MB + USB + Ethernet + at least 800x600x8? - make a proper Linux workstation

1 CPLD or 2 CPLDs in concert are a DMA list processor?
