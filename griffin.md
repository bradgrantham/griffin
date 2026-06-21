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

## Summary

* **CPU:** 68010 primary (68000 is a drop-in), 14 MHz SYSCLK
* **RAM:** 16-bit-wide async SRAM, ~12 MB usable.  Two AS6C6416 (4M×16 = 8 MB each, 55 ns) populate 16 MB, but only 0x000000–0xBFFFFF (12 MB) is addressable — the top 4 MB is peripherals.  Decode *simplifies*: GLUE drives 2 chip selects (chip0 = A23==0, chip1 = A23 & ~A22) instead of 4 banks, /UB,/LB from UDS/LDS — frees 2 GLUE pins and drops the bank-population/sizing logic
* **ROM:** 2× SST39SF040 in PLCC sockets (1 MB, 16-bit, 5V, 70 ns).  Bench-program the first image in a programmer; in-circuit reflash thereafter via a GLUE-asserted ROM-region write strobe (the flasher runs from RAM — these parts are not read-while-write).  Stretch goal: first-program / OTA over JTAG by driving the CPLD-chain boundary scan (CPU held in reset; address from ENGINE, data from VIDEO/GLUE, ~CS + ~WE from GLUE decode)
* **CF:** 16-bit True IDE PIO, with IOWR gated by AS in GLUE - **Needs glue.v change to support R/nW-AS -> yields IOWR**
* **RTC:** DS3231 (TCXO) on I²C via the DUART's spare OP/IP pins — off the bus, **zero GLUE logic** (no A17, no RTC chip-select/strobes, no wait states).  5 V like the DUART so no level translation.  SCL = an OP pin (push-pull is fine — the DS3231 doesn't clock-stretch); SDA is open-drain — an OP pin drives the gate of a small N-FET (e.g. 2N7002, SOT-23) — drain on SDA, source to GND — to pull SDA low, and an IP pin reads the line back; 4.7 kΩ pull-ups to 5 V.  Optional INT#/SQW → a DUART change-detect IP for a 1 Hz / alarm IRQ.  (Chose this over the BQ3285, whose 146818-style multiplexed bus would have cost GLUE an A17 input + AS/DS strobe generation + 3 pins.)
* **GLUE ATF1508** — address decode, ~R/W + write-strobe generation, autovector ~VPA, PS/2 frame engine. 
  * separate DEBUG LED pin + NPN driver; used liberally from CPU for boot codes / steady-state (video ISR) / double-fault (GLUE)
  
  * decode ~AUDIO_SEL (read+write qualified) for the AUDIO CPLD and feed nAUDIO_IRQ into the priority encoder at level 2; add a flash write strobe ~ROM_WE (gated by CONFIG.FLASH_WE_EN, default write-protected)

  * net Rev 2 effect — GLUE comes out *smaller* than Rev 1: RAM 4 bank-selects → 2, RTC moves off-chip (DS3231/I²C, no A17), and the only addition is ~ROM_WE
  
  * PS/2 keyboard frame engine stays in GLUE (RX one IRQ/byte, TX shifted on the device clock); no mouse port
  
  * wire up some jumpers to do things like set clock speed as input pins are available


* **Serial I/O**
  - boot/panic output goes through DUART Channel A (no more bit-bang); pre-DUART failures are LED-blink only

  - XR68C681 on-board : Channel A console, Channel B 2nd is peripheral serial port; C/T gives 100 Hz systick (level 5) + a configurable timer ISR.

  - pair of pins for 2nd UART for e.g. ESP32 communication, PPP, ... - as high baud rate as system can drive

  - DUART OP/IP pins bring out RTS/CTS flow control + change-detect GPIO

  - configurable timer ISR

  - Also bring out I/O pins
* **FT4232H on-board** (USB-C, SMT): one chip = 4 independent ports over one cable
  - Channel A (MPSSE) = JTAG to the CPLD chain (program CPLDs + the stretch-goal flash-over-JTAG)
  - Channel B = console UART ↔ XR68C681 Channel A; Channel C = 2nd UART ↔ XR68C681 Channel B
  - Channel D = host-driven board control: assert CPU ~RESET / ~HALT (and the flash-bootstrap enable) so the PC can reset/reflash hands-off.  ~RESET/~HALT are open-drain 5V lines with pull-ups — drive them low to assert (open-drain buffer / FET), release otherwise
  - FT4232H is 3.3V and not 5V-tolerant: 74LVC245 buffers (3.3V rail, 5V-tolerant inputs) on every line that drives *into* the FT4232H (CPLD TDO, DUART TxD, any ~RESET/~HALT readback); 3.3V→5V lines (TCK/TDI/TMS, FTDI TXD) drive direct
  - support parts: 12 MHz crystal, 3.3V LDO off the 5V rail, decoupling; 93LC56 EEPROM optional (custom USB identity only — not needed for JTAG/UART)


* **Video — VGA ** 640x480@60 1bpp through shared SRAM

  - 25.175 MHz pixel clock (separate oscillator from the 14 MHz SYSCLK), 640x480@60
  - Pair of 7200 shift registers on the bus (may need transceivers because of capacitance?)
    - Possibly need resistors inline and capacitors to ground based on jumper-to-solderless experiment
  
  
  * VIDEO CPLD drives timing and sync; pulls bytes from 7200 shift registers
    * Pull even then odd bytes to limit macrocells
    * ENABLE bit in register, enable *after* ENGINE so FIFO is partially filled at VIDEO start
    * VBLANK IRQ
    * Palette is in-band: the first word of each scanline carries {fg,bg} (R3G3B2 each) and VIDEO latches it from the FIFO during hblank — there is no PALETTE CPU register
    * Bonus bitfile addition if it fits : 2bpp 320x240 with 4 palette entries
  * ENGINE CPLD drives 16-bit DMA, latches pair of 7200 shift registers
    * SOURCE_PAGE register = A[23:16] of the 64K-aligned framebuffer base
    * streams VIDEO_WORDS_PER_LINE words/scanline in ENGINE_WORDS_PER_BURST-word bus bursts paced to HBLANK, latching into the 7200s
    * ENABLE bit in register - enable *before* VIDEO
    * per-line streaming lines up with VIDEO scanout, so the CPU can flip SOURCE_PAGE between frames for double buffering


* **Audio — dedicated ATF1504AS (PLCC44) at 0xFC0000** (replaces the Rev 1 '373 latch):
  - 2× 7202 FIFO + 2× 8-bit R2R DAC.  CPU D[15:0] wires straight to the FIFOs (left = D[15:8], right = D[7:0]); the DACs hang off the FIFO outputs
  - the CPLD only generates ~W (full-word writes to the A17-high alias half), ~R (from a 12-bit sample-rate divider clocked by SYSCLK on a GCLK), and the latched ~HF → nAUDIO_IRQ (level 2)
  - CPU fills the FIFO in IRQ checking HF; baseline ~15.7 kS/s stereo 8-bit (rate set by the divider).  No mono packing for now
  
  - self-clocked from the AUDIO CPLD's own divider, so no VIDEO line-rate signal is needed
  
  - **Need new audio.v and registers in YAML and schematic**
  

## Board changes

- [ ] Schematic (+PCB if necessary)
  - [ ] Need BOM output but some way to select “I have these already”.  
  - [ ] Compile bitfiles for CPLDs and let fitter assign pins in order to let macrocell count be minimized
  - [ ] ROM: 2× SST39SF040 (512K×8, 5V, 70 ns) in PLCC sockets → 1 MB, 16-bit, socketable, in-stock
    - [ ] GLUE asserts a ROM-region write strobe ~ROM_WE — gated by a new CONFIG.FLASH_WE_EN bit (default off = write-protect) and timed off UDS/LDS; flash /CE←nROM_SELECT, /OE←nR_W, /WE←nROM_WE; word-writes only (shared WE across the two byte-wide chips); flasher stub runs from RAM (not read-while-write)
    - [ ] image-zero is bench-programmed in a TL866-class programmer; in-circuit reflash thereafter
    - [ ] stretch: first-program / OTA over JTAG via CPLD-chain boundary scan (no programmer, no socket pull)
  - [ ] RAM: 2× AS6C6416 (4M×16, 55 ns) → 16 MB populated, 12 MB addressable (0x0–0xBFFFFF); AS6C4008-pair footprint as in-stock fallback.  No SDRAM/PSRAM, no memory controller
    - [ ] GLUE decodes 2 chip selects (chip0 = A23==0, chip1 = A23 & ~A22) instead of 4 banks; /UB←nUDS, /LB←nLDS, /OE←nR_W — frees 2 GLUE pins, drops bank-sizing
  - [ ] Pullups
    - [ ] JTAG lines
    - [ ] Any CPU lines that may lead or not be driven - 4.7K HALT
    - [ ] Any inter-IC signals that might cause stalls or floating behavior
  - [ ] More signals between CPLDs??
  - [ ] Decoupling caps for every +5V/GND pair especially CPLDs
  - [ ] Source termination (33-100Ω series at CPLD output) on fast control signals to FIFOs — /RE_EVEN, /RE_ODD, /W, q8_toggle.  Rev 1 bringup: unterminated bodge wires caused /RE ringing to cross threshold at FIFO input, doubling reads and corrupting display (image creep up/left).  Workaround: 10pF caps from each /RE pin to GND at the 7200s — stable but not rock solid.
  - [ ] GND, +5V, D, A, WRITE_LO, WRITE_HI, IO/VIDEO/ENGINE/AUDIO select/latch, nVPA to test points, basically bring out every inter-IC signal
    - [ ] Use a pin header expecting Dupont jumpers to logic analyzer or use a jumper to a scope probe
    - [ ] Make the pin header be 2xN, down each side silk screen the signal at the pin
    - [ ] Put in lots of holes for ground test points around the board
    - [ ] Bring out pins from GLUE, if there any left, for the purpose of debugging
    - [ ] Put all the test points in female header suitable for plugging Dr. Guzman's analyzer into directly - test how it works to have two ganged together and make the headers that shape
  - [ ] Pullups on PS/2 clock and data lines
  - [ ] Make SYSCLK go into a GCLK on CPLDs especially GLUE
  - [ ] Audio: dedicated ATF1504AS (PLCC44) at 0xFC0000 + 2× 7202 FIFO + 2× 8-bit R2R DAC, stereo (one 16-bit write; L=D[15:8], R=D[7:0])
    - [ ] CPLD generates ~W (full-word writes to A17-high alias), ~R (12-bit SYSCLK divider on a GCLK), and latched ~HF → nAUDIO_IRQ (level 2)
    - [ ] GLUE: replace write-only ~AUDIO_LE with a read+write-qualified ~AUDIO_SEL routed to the AUDIO CPLD
  - [ ] Wire all CPLDs into the JTAG chain (GLUE, VIDEO, ENGINE, AUDIO) — also the path for the stretch-goal boundary-scan flashing
  - [x] Put in a driver for a separate DEBUG LED and pin so it doesn’t interfere with debug out voltage level  
    - [ ] small NPN like a 2N3904 or SOT-23 MMBT3904. Collector to \+5V through the LED and resistor, base to the DEBUG\_OUT pad through a 1K–10K resistor, emitter to ground.
  - [ ] Put USB-C with two sink resistors on the board - rev1 operates at .84A according to inline USB-C meter
  - [ ] Much more attention to analog components - redesign the VGA analog circuitry to be robust (composite/NTSC dropped for Rev 2)
  - [ ] XR68C681 DUART : DIP-40, proper bus drivers, two UARTs, C/T, parallel I/O
    - [ ] Wire D0-D7, R/~W directly to the XR68C681
    - [ ] Wire A1-A4 to RS1-RS4 (16 registers, 4 select lines)
    - [ ] Wire ~RESET directly from ~RESET net (active-low; AT89S52 RST was active-high via IO_RESET from GLUE — no longer needed)
    - [ ] 3.6864 MHz crystal on X1/X2 for clean baud rate division (replaces MCU Y2 crystal)
    - [ ] Tie ~IACK high (autovectors); frees one GLUE pin
    - [ ] Channel A: terminal UART (replaces DEBUG_OUT/DEBUG_IN bit-bang); Channel B: ESP32 or other network device
    - [ ] RTS/CTS flow control on both channels: OP0/OP1 + IP0/IP1
    - [ ] DS3231 RTC on I²C from spare DUART pins: SCL=OP2 (push-pull OK — DS3231 doesn't stretch); SDA driven low by OP3→N-FET gate (2N7002, drain on SDA), read back on IP2; 4.7 kΩ pull-ups to 5 V; optional INT#/SQW→IP change-detect for 1 Hz/alarm IRQ
    - [ ] leaves OP4-OP7 / IP3+ spare
  - [ ] FT4232H on the board (USB-C, SMT): 4 ports over one cable — A=JTAG (MPSSE), B=console UART (XR68C681 ChA), C=2nd UART (XR68C681 ChB), D=board control
    - [ ] Channel D GPIO asserts CPU ~RESET / ~HALT (open-drain onto the 5V pull-up lines) and the flash-bootstrap enable, so the host can reset/reflash the board hands-off
    - [ ] 74LVC245 level translators (3.3V) on the 5V→3.3V lines into the FT4232H: CPLD TDO, DUART TxD, any ~RESET/~HALT readback; 3.3V→5V lines (TCK/TDI/TMS, FTDI TXD) go direct
    - [ ] 12 MHz crystal, 3.3V LDO, decoupling; 93LC56 EEPROM optional (custom USB identity only)
    - [ ] keep a 2×5 JTAG header in parallel for an external programmer / FT4232H isolation
  - [ ] CF card
    - [ ] symbol is junk - redo it.
    - [ ] CF card IOWR should be gated by AS.
      - [ ] CF card latches on rise of IOWR
      - [ ] If just the 68000's R/~W passed through, then AS is long gone and data may be junk at time of rise of IOWR.  Fix is to combine them through GLUE.
    - [ ] CF card to 16 bits
    - [ ] CF card schematic has weird pin numbers
  - [ ] RTC: DS3231 (TCXO) + coin cell on I²C via DUART OP/IP pins (see DUART item) — off the bus, no GLUE logic, no A17, 5 V (no level shift)
- [ ] PCB only
  - [ ] SMT parts and footprints? - CF, RAM (AS6C6416 TSOP), XR68C681, the AUDIO/ENGINE/VIDEO/GLUE CPLDs
  - [ ] CF card DMACK to +5, CF card CS0 and CS1 are swapped!!  Fix them for now in Verilog, revisit Verilog and PCB for rev 2
  - [ ] Do more of a hub-and-spoke kind of model, run bus and signals across from CPU, put peripherals above and below with vertical taps
  - [ ] PS2 footprint and pin mapping was all wrong (single keyboard port)
  - [ ] Headphone jack pads - drill partial holes?  
  - [ ] RCA jack retainer feet - drill partial holes?
  - [x] Route A18 to GLUE instead of A6  
  - [x] Wire GLUE VPA back to the CPU in place of ENGINE\_IACK
  - [ ] Decoupling for ROM is too close to the socket if I will be using ZIF - need ZIF footprint
  - [ ] Flip FTDI - it's 180 degrees so I have to currently put FTDI upside down onto 90-degree header

## Possible Linux target

m68k NOMMU build - may have bitrotted versus Coldfire

2MB not likely to be capable of much, 4MB is better, 12MB would be best (use all address space available other than peripherals)

https://github.com/AcceleratedLinux/accelerated-linux

https://github.com/fifteenhex/m68kjunk

# Rev 3

68030 + 68882 + >=32MB + USB + Ethernet + at least 800x600x8? - make a proper Linux workstation

1 CPLD or 2 CPLDs in concert are a DMA list processor?
