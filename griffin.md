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

Dated decisions, measurements and bring-up investigations: [griffin.log](griffin.log).

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

**Rev 2: power is a 5.5mm barrel jack, not USB-C — case wants a round cutout.**

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

Rev 1: USB-C power with switch inline between supply and USB-C port; "two 5.1k pull-downs on both CC pins" — measured 0.84 A draw.

**Rev 2: 5.5mm x 2.1mm center-positive barrel jack**, regulated 5V ≥2.5A adapter, inline switch kept.  Replaces USB-C: a plain-Rd USB-C sink is only *entitled* to 500 mA without reading the source's Rp advertisement or negotiating PD, and the rev-2 budget is ~1.3–1.7 A typ (see rev2-electrical-review.md §8).  Silkscreen the contract at the jack: "5V regulated 2.5A+ center positive".  Have adapters, can make/buy more; a CH224K-style PD trigger module with a barrel pigtail turns any PD charger into a compliant supply if ever wanted.

## Power-on reset and user reset button

DS1233-5 reset supervisor (rev 2; rev 1 had only the R2/C3/SW1 RC + button).  Datasheet (041002) facts: open-drain RST with internal 5 kΩ (3.75–6.25 k) pull-up, trip point 4.50–4.74 V (typ 4.625), 350 ms stretch, IOL 8 mA.

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
* PS/2 frame engine: GLUE assembles a full 11-bit PS/2 frame in hardware and raises one level-4 IRQ per byte on RX (byte in PS2\_RX\_DATA, parity/framing flags in PS2\_STATUS for firmware to check), and on TX shifts a host-to-device frame out on the device clock (the PS2\_TX\_DATA write presents the start bit and releases CLK; ACK sampled into PS2\_STATUS.TX\_ACK; TX\_DONE IRQ on completion).  Firmware-computed odd parity is carried in PS2\_TX\_DATA address bit 1 (0x09 = parity 0, 0x0B = parity 1).  Half-duplex, one shared shifter.  
* Serial is the 68681 DUART (Channel A, 115200 8N1), not GLUE.  crt0 brings the DUART up early and all boot/exception/panic prints go through it (stack-free putchar, TXRDY-poll with timeout to LED blink); pre-DUART failures are LED-blink only.  
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

For PCB Rev1, serial is the 68681 DUART and PS/2 is a GLUE frame engine.  Current status:

* *Working* serial via 68681 DUART Channel A, 115200 8N1: interrupt-driven RX + polled TX (`duart_putchar`); crt0 brings it up early so boot/panic output uses it too
  * Need to get to reliable streaming serial so I can send and receive data to some kind of network device e.g. esp32 (the DUART's FIFO + flow control should get there)
* *Working* SYSTICK at 100Hz from the 68681 C/T (level-5 IRQ)
* *Working* PS/2 RX as a GLUE frame engine: one level-4 IRQ per assembled byte
* *Working* PS/2 TX through the same GLUE frame engine (host-to-device, device ACK sampled); firmware-computed parity carried in the PS2_TX_DATA write address

The Rev 1 AT89S52 IO MCU was abandoned: its weak P2 pull-ups could not drive the data bus against whatever held it during IO MCU cycles (investigation in griffin.log).  The XR68C681 replaced it on a DIP carrier.

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

## Possible new hardware features

Generalized ENGINE — to look into:

* Stream into VIDEO, AUDIO, or COPY FIFOs from RAM, ROM, CF
* Stream out of COPY FIFO to RAM
* Optional block on HF for VIDEO, AUDIO FIFOs
* Fixed size?  Or count for COPY ops?
* Net impact: as bus master ENGINE reaches the audio FIFO / CF through normal GLUE-decoded cycles, so mostly no new nets — the exception is observing the FIFO HF flags it must block on (AUDIO ~HF at minimum); decide with the "more inter-CPLD signals" layout item

## PORTS — the fourth CPLD

The PS/2 mouse gets its own CPLD, PORTS, together with the joysticks, the paddle counters and the audio FIFO pop: two PS/2 frame engines do not share one ATF1508, so the keyboard stays in GLUE and the mouse leaves; reading the DE-9s and counting the paddle ramps costs almost nothing in a CPLD that already exists, which retires the 74HCT245 / 74HC590 chips and the DUART OP4/OP5 dump dance; and a programmable audio divider is not worth a chip when a line-rate tick from the timing chip gives the sample rate the design already assumed.  Rev 2 is therefore GLUE, ENGINE, PORTS, PIXEL and COMPOSITOR on ATF1508AS PLCC84 plus TIMING on an ATF1504AS PLCC44, all with frozen pinouts.  Headroom: ENGINE has the only real logic reserve and spare I/O (a generalized COPY-FIFO or a second DMA channel goes there); PORTS has spare pins but little logic, so a small connector-facing addition goes there and anything larger wants ENGINE.  Fit measurements: griffin.log (2026-07-28, 2026-07-30).

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
* **Reset:** DS1233-5+-ND supervisor + user reset button.  ~RESET/~HALT must accept external open-drain assertion from the debug header (backup bus-master role) without fighting the supervisor — the DS1233's open-drain RST + internal 5 kΩ pull-up tolerates external lows
* **RAM:** as Rev 1 — 4 MB as 4 banks × 2 of AS6C4008 (512K×8, DIP-32, socketed, bin parts).  GLUE drives nRAM_[1-4]_SEL (banks by A20/A21) and WRITE_LO/WRITE_HI byte write strobes; /OE←nR_W.  Banks are fully populated, no additional code to manage partial population.
* **ROM:** 2× SST39SF040-70-4C-PHE (512K×8 5 V NOR flash, DIP-32, socketed) = 1 MB on the 16-bit bus, one per byte lane — 8× the Rev 1 W27C512 pair, which the firmware had already filled to 91%.  Single 4.5–5.5 V supply for read *and* program/erase (Vpp on-chip), so the TL866II+ programs them in the bare DIP-32 socket; 70 ns keeps the Rev 1 one-wait-state DTACK.  Decode, /OE←nR_W and the boot overlay are all unchanged from the W27C512 build — same A23 & ~A22 nROM_SELECT, the 1 MB image just mirrors 4× instead of 32× through the 4 MB window.  **New in Rev 2: the CPU can reflash in circuit.**  GLUE drives ~ROM_WE to both WE# pins, asserted only for (A23 & ~A22) & ~R/W & AS & UDS & LDS & CONFIG.FLASH_WE_EN (resets to 0 = write-protected).  Demanding both data strobes makes byte writes produce no strobe at all, so nothing can feed a command to one x8 chip and desync the pair; qualifying on the raw region instead of nROM_SELECT keeps overlay-window writes off the flash regardless of the overlay bit.  JEDEC x8 command bytes are duplicated into both halves of each word (unlock 0xAAAA→0x80AAAA, 0x5555→0x805554), one word write programs both lanes in parallel, and DQ7/DQ6 are polled per lane; sector erase is 4 KB per chip = 8 KB of CPU space.  Flasher runs from RAM (.ramtext) with the overlay off — the parts return status, not instructions, during an embedded operation.  Because they stay socketed, a bricked image is recovered by pulling them and using the bench programmer; Rev 2 has no bus-master bootstrap path (that arrives with the RP2350B bridge in Rev 3).
* **CF:** 16-bit True IDE PIO; IOWR gated by AS (needs a glue.v change: R/~W + AS → IOWR).
* **RTC:** DS3231 (TCXO) on I²C via the DUART's spare OP/IP pins — off the bus, zero GLUE logic (no A17, no chip-select/strobes/wait-states), 5 V so no level translation.  SCL = an OP pin (push-pull is fine; DS3231 doesn't clock-stretch); SDA is open-drain via an OP→N-FET pull-down with an IP reading back.  Optional INT#/SQW → a DUART change-detect IP for a 1 Hz/alarm IRQ.  (Chosen over the BQ3285, whose 146818-style multiplexed bus would have cost GLUE A17 + AS/DS strobes + 3 pins.)
* **GLUE ATF1508:** address decode (nRAM_[1-4]_SEL, nROM_SELECT, CF incl CS0/CS1 + AS-gated IORD/IOWR, DUART, ~PORTS_SELECT), DTACK/wait-state generation for everything including PORTS, WRITE_LO/WRITE_HI byte write strobes, ~ROM_WE for in-circuit flash programming (word writes only, gated by CONFIG.FLASH_WE_EN), the boot ROM overlay + CONFIG register, autovector ~VPA, the PS/2 **keyboard** frame engine (RX one IRQ/byte, TX on the device clock — the keyboard stays here; the mouse is a second engine over in PORTS), ~PORTS_IRQ into the priority encoder at level 2, and a separate DEBUG-LED driver used for boot codes / video-ISR heartbeat / double-fault.  ~ENGINE_SELECT is a GLUE output on a real net (ENGINE takes the select on its pin 84), and ~ENGINE_IRQ is a level-3 input.  Nothing is decoded at 0xC00000 or 0xE00000; those regions BERR by timeout.
  * Pinout frozen in the `//PIN:` block at the bottom of glue.v (pin-full: one spare I/O); the design needs `xor_synthesis` to fit and drops `-strategy debug`.  Fit history in griffin.log.
* **Serial I/O — XR68C681 DUART:** Channel A = console (115200 8N1; boot/panic output, no more bit-bang — pre-DUART failures are LED-blink only), Channel B = 2nd serial brought out to a TTL header (ESP32 / PPP / an RS232 level-shifter module) — deliberately not routed through USB.  C/T = 100 Hz systick (level 5) + a configurable timer ISR.  OP/IP pins host RTS/CTS flow control on both channels and the DS3231 I²C; allocation table in griffin.yml; spares remain.  (OP4..OP7 go to TIMING as a reserved mode/rate selector.)
* Console UART header
* JTAG programming 1×6: TCK/TMS/TDI/TDO + 5 V + GND) 
* **Debug headers — LA access + "PCB design failed" backup:** two 2×15 0.1" headers laid out to **mirror the LogicAnalyzer level-shifter board's own 2×15 pinout** (24 channels + GND + 3V3 + 5V-reference + 2 external-trigger pins each), so the two on-hand 5 V-tolerant analyzers plug straight on and daisy-chain via their own 3-pin chain connectors into one synchronized 48-channel capture.  Exact channel-to-pin mapping, mating gender, and orientation come from the gusmanb KiCad files at schematic time — the wiki shows it only as a diagram (https://github.com/gusmanb/logicanalyzer/wiki/02---LogicAnalyzer-Hardware).  Channel plan (47 bus/control signals + SYSCLK = 48, an exact fit): **header 1 "what happened"** = D0–D15, ~AS, ~UDS, ~LDS, R/~W, ~DTACK, ~RESET, ~HALT (exactly 23); **header 2 "where"** = A1–A23 + SYSCLK (24).  5V-reference pins fed from the Griffin rail (analyzer's onboard jumper removed → external reference); 3V3 pins NC; trigger pins NC/test pads.  Roles: (1) *Observe:* whole-bus captures as above.  (2) *Backup bus-master:* every signal a master needs is on these same two headers — hold ~RESET+~HALT (68000 tri-states A/D/strobes; GLUE still does its normal decode, so the flash sees ordinary write cycles) - Bus-cycle mechanics for whoever masters: bit-banged/PIO read_word/write_word (honor ~DTACK or run conservatively slow) 
* **Video — VGA 640×480@60, 12-bit R4G4B4, display-list driven ("super-engine").**  The Rev 1 pair (in-band-header VIDEO + page-streaming ENGINE) is retired; the path is four chips: **ENGINE → {PIXELS, VIDCMD} FIFOs → PIXEL (+TIMING) → COMPOSITOR → resistor DAC**.  Semantics live in vidcmd-spec.md; the executable model + budget checker is super-engine/; shared numbers and board support parts are in griffin.yml `constants:` and `interfaces:`.
  * **ENGINE** (engine.v, Rev-1 pinout): walks 4-word descriptors from the top 64K of RAM.  Each descriptor says: wait for hblank or don't, read N words from this address, and pulse this FIFO's write strobe (PIXELS / VIDCMD / AUDIO, one-hot) — or strobe nothing, which just burns bus time to place a later deposit.  The last one raises the level-3 IRQ and stops; the CPU's vsync ISR re-arms with the other buffer.  Rule learned twice: *any* RAM a list points at is frame-owned — double-buffer the lists and their data.
  * **PIXEL** (pixel.v): unpacks the PIXELS FIFO — pure pixel bits now, the 4-byte in-band header is gone — in 1bpp (40 words/line) or 2-bits-per-clock micro-HAM (80 words/line), through 12-bit palette/held registers, out as R4G4B4.  On FIFO underrun it re-shifts the last word rather than erroring, which turns short fills into a compression feature: one 0x0000 word paints background to end-of-line, or end-of-frame.
  * **TIMING** (timing.v, ATF1504 PLCC44, testbench timing_tb.v): raster counters, syncs, /RS at vsync, the three event strobes PIXEL consumes, and the board's other time bases — HBLANK to ENGINE, nVSYNC_IRQ to GLUE, and the PADDLE_TICK / AUDIO_TICK square waves (toggle per line, PORTS counts falling edges: 15.734 kHz) — plus four pins reserved for DUART OP4..OP7 as a future mode/rate selector.  Split out because PIXEL+TIMING together do not fit one ATF1508 (pixel_combined.v is the kept negative result).  Every stage's fixed pipeline delay is cancelled here by firing comparators early: the lead constants (COMPOSITOR_LEAD 2, DAC_LEAD 4, …) are *derived* in griffin.yml, not tuned on hardware.
  * **COMPOSITOR** (compositor.v; its iverilog testbench is the timing-semantics spec): executes the VIDCMD instruction stream — RUN spans (passthrough, two held colors, or eight saturated RUN_COLOR literals) and SET, which writes its own held colors or forwards a 12-bit value into PIXEL's registers over a valid/target/commit handshake.  Per-line palettes, pixel-exact mid-line color changes, sprites-as-spans, and full-screen RLE images are all just authored stream content.  When the FIFO runs dry at a record boundary the current span continues to end of line — so lines with nothing to say cost zero words.
  * **No CPU bus on PIXEL, TIMING, or COMPOSITOR.**  Every register they have is reached only by SET instructions the CPU authors into RAM and ENGINE DMAs into the VIDCMD FIFO.  vsync IRQ (level 6) moves from VIDEO to TIMING; ENGINE's level-3 IRQ is genuinely driven at last (list-completion handshake).
* **Joysticks — 2× DE-9, Atari 2600-style, straight into PORTS.**  the switch lines land on PORTS pins and read back as two byte registers (JOYSTICK_PORT_1/2 at PORTS 0x01/0x03); the '245s, the ~JOY_OE strobe and the 0xC40000 slot all go away, and the ESD boundary becomes series R + clamps at the connectors.  Two byte reads instead of one `move.w`; bits active-low (0 = switch closed).  U/D/L/R on DE-9 pins 1–4, fire on pin 6; also wire pins 5/9 (Sega Master System pad button 2 works, pads still manufactured), leaving spare bits per lane.  Pull-ups on every switch line, +5 V on pin 7 via polyfuse, GND pin 8.  Polled at 60 Hz in the vsync ISR — no IRQ, no CPLD state machine; registers defined in griffin.yml so the emulator can map them to keys before the board exists.
  * **Paddles — one port, 2 paddles.**  A 2600 paddle is a 1 MΩ pot from +5 V (pin 7) to pin 5/9 with fire on pins 3/4, so the connector wiring above already covers it.  Per paddle: ~10 nF cap to GND + 2N7000 drain FET on the pot line.  PORTS runs an 8-bit saturating upcounter per paddle at the full VGA line rate (still exactly the 2600's method: count while the RC ramp holds the pin below the input threshold, freeze at the crossing; t ≈ 0.69·RC, size C for ≈255-line ≈8.1 ms full scale) and the two FET gates come off one PADDLE_CONTROL.DUMP bit instead of the DUART OP4/OP5 pair.  CPU cost per frame is two byte reads and one byte write; still immune to DMA/IRQ jitter.  Expect software calibration (the input threshold varies part to part, and the pot parallels the pin-5/9 pull-up).  Paddle-vs-stick is a per-port software mode, as on the real 2600.
* **PORTS ATF1508 — the fourth CPLD, 0xFC0000:** PS/2 **mouse** frame engine (same design as GLUE's keyboard one, same register offsets so one firmware driver serves both), both **joystick** ports read as bytes, both **paddle** counters + the dump control, and the **audio FIFO pop** strobe, ~RS control and ~EF status.  It carries no bus timing of its own: GLUE hands it a cycle-qualified ~PORTS_SELECT and answers DTACK, exactly as for CF.  Its time bases are two nets from TIMING — **PADDLE_TICK and AUDIO_TICK**, square waves toggling once per line, synchronized on arrival and counted on falling edges (15.734 kHz events for both).  No prescaler, no programmable divider, anywhere.  cpld/ports/ports.v takes its register decode from the generated griffin.yml defines and its pinout is frozen in its `//PIN:` block; it does not use `xor_synthesis` (measured to make no difference — griffin.log).  See pcbv2-ports-design.md.  This is what retires the 2× 74HCT245 and 2× 74HC590.
* **Audio - FIFO** (replaces the Rev 1 '373 latch): 2× 7200 FIFO (256×9; 256 pairs = 16 ms of buffer, ~HF at 128 = ~123 Hz refill, cascadable via XI/XO if depth is ever wanted) + 2× 8-bit R2R DAC, stereo (L = D[15:8], R = D[7:0] straight to the FIFOs).  ~W is ENGINE's AUDIO_FIFO_W descriptor strobe: the display list deposits sample words by DMA and there is no CPU-mapped write path.  **PORTS** generates ~R once per AUDIO_TICK event from TIMING and drives both ~RS (AUDIO_CONTROL.RESET, held from power-on until released; also the flush); ~EF goes to PORTS as pollable EMPTY status, ~HF is unconnected, and there is no audio interrupt — the level is dead-reckoned and a missed frame is detected from the vsync latch; baseline ~15.7 kS/s; no mono packing.  Samples are **two's complement** (0x00 = silence): CD4049 inverters flip each channel's Q7 into the unsigned ladders.  Output: dual 10kA volume pot, LM386 + internal 8-ohm speaker fed from the jack's NC switch contacts (plug insertion mutes; rev-1 buzzer retired).
* **Interrupts (autovector):** vsync 6 (TIMING's dedicated nVSYNC_IRQ output, pin 31, to GLUE pin 65 — polarity fixed regardless of raster mode — edge-latched there, W1C via GLUE VSYNC_CLEAR; TIMING has no bus to host the ack), DUART 5, PS/2 keyboard 4, ENGINE 3, PORTS 2 (mouse only).  ENGINE still ties ~ENGINE_IRQ high, so level 3 is inert today; the GLUE pin and the net exist so enabling it later is a bitfile change, not a respin.
* **Address map:** RAM 0x000000–0x3FFFFF; **ROM window 0x800000–0xBFFFFF** (decode = A23 & ~A22, two literals; the 1 MB SST39SF040 image mirrors 4× through the 4 MB window); all peripherals under A23 & A22: ENGINE 0xD00000 (GLUE decodes ~ENGINE_SELECT for it), GLUE 0xF00000, CF 0xF40000, DUART 0xF80000, **PORTS 0xFC0000**.  0xC00000–0xCFFFFF and 0xE00000–0xEFFFFF are undecoded (BERR by timeout); the audio FIFOs are reached only by ENGINE DMA.  Registers and constants in griffin.yml.

## Board changes

*(KiCad watch-list — footprints, pinouts, nets, placement, SI.  Function lives in Rev 2 components above; link, don't restate.)*

**CPLD pin numbers are not in this document.**  The authoritative Rev 2 pinout for each part is the `//PIN:` block at the bottom of its Verilog — cpld/glue/glue.v, cpld/ports/ports.v, cpld/compositor/compositor.v, cpld/pixel/pixel.v and cpld/pixel/timing.v (all **FROZEN**), and cpld/engine/engine.v (carried from the Rev 1 hand assignment).  Route from those files, not from a copy — a copy drifts.  A netlist change that makes the fitter want a different placement on GLUE or PORTS is a respin, not a re-fit.

### Footprints & sockets
- [ ] ROM: 2× SST39SF040 DIP-32 in sockets (one per byte lane, 1 MB ×16 — note DIP-32, **not** the Rev 1 DIP-28 W27C512 footprint); /OE←nR_W, /CE←nROM_SELECT, WE#←~ROM_WE (new net, both chips in parallel); initial images via the bench programmer, thereafter self-reflash
- [ ] RAM: 8× DIP-32 sockets, 4 banks × 2 of AS6C4008 (populate contiguously from bank 1, as Rev 1); /OE←nR_W, WRITE_LO/WRITE_HI byte strobes + nRAM_[1-4]_SEL from GLUE
- [ ] Audio: 2× 7200 FIFO + 2× R2R DAC on the board (replaces the Rev 1 '373 + write-only ~AUDIO_LE); ~W from ENGINE's AUDIO_FIFO_W strobe pin, **~R and ~RS from PORTS, ~EF to PORTS**, ~HF unconnected
- [ ] Video FIFOs: 2× 7200 on the board natively (retires the piggyback bodge of video-fifo-wiring.md); termination in SI section below
- [ ] VGA: DE-15 connector; remove the composite/NTSC jack and any NTSC clock provisions — Rev 2 is VGA-only
- [ ] DUART: XR68C681 DIP-40 native (retires the DIP-carrier bodge); A1-A4→RS1-RS4, D0-7 + R/~W direct, ~RESET from ~RESET net, ~IACK tied high (autovectors), 3.6864 MHz crystal on X1/X2 (clearance if ZIF)
- [ ] Console: DUART Ch A TTL header (FTDI-cable pinout); Ch B TTL header (RS232 level-shifter module optional)
- [ ] RTC: DS3231 + coin-cell holder; SCL=OP2, SDA via OP3→2N7000 (drain on SDA) + IP2 readback
- [ ] PS/2: fix footprint + pin mapping — **two ports now**: keyboard CLK/DATA to GLUE, mouse CLK/DATA to PORTS
- [ ] PORTS: ATF1508AS PLCC84 socket + JTAG chain; nets ~PORTS_SELECT / ~PORTS_IRQ / A4-A1 / ~LDS / R~W / D7-D0 to GLUE and the bus, **PADDLE_TICK (TIMING 17 → PORTS 44) and AUDIO_TICK (TIMING 18 → PORTS 45)** as its only time bases, ~R, ~RS and ~EF to the 7200 pair, mouse PS/2, both DE-9s, PADDLE_DUMP to the FET gates — pin numbers from the frozen `//PIN:` block in cpld/ports/ports.v
- [ ] TIMING fan-out (pins from the `//PIN:` block in cpld/pixel/timing.v): HBLANK (pin 19) → ENGINE pin 2; nVSYNC_IRQ (pin 31) → GLUE pin 65 (VGA_VSYNC no longer tee'd); VGA_HSYNC/VGA_VSYNC only to the 74AC541s; DUART OP4/OP5/OP6/OP7 → TIMING pins 20/21/24/25 (reserved, no logic yet — route them anyway); leave GCLK2 (pin 44) free with an optional oscillator footprint; the remaining TIMING spares are bitfile-usable later only if routed to reachable copper (header or test points)
- [ ] ~ENGINE_IRQ: ENGINE pin 5 → GLUE pin 68; needs a pull-up like the other IRQ nets (see Pull-ups)
- [ ] No 74155 and no direct-bus region in Rev 2 (deleted with the CPU audio path); ~ENGINE_SELECT *is* a net (GLUE pin 39 → ENGINE pin 84); CF CS1 stays routed from GLUE
- [ ] Joysticks: 2× DE-9 male PCB-mount, switch lines **straight into PORTS** — no '245s; bussed pull-up networks, polyfuse on pin-7 +5V, optional series R + clamps between connector and the CPLD
- [ ] Paddles (port 1 only): 2× {~10 nF film cap to GND + 2N7000 drain, gate←PADDLE_DUMP from PORTS} on DE-9 pins 5/9, pin 5/9 also to PORTS as the count-enable senses — no '590s; all DNP-able, a sticks-only build omits the caps/FETs
- [ ] everything THT / socketed where practical — Rev 2 stays bodgeable by intent
- [ ] Headphone-jack pads / RCA retainer feet — partial holes?
- [ ] JTAG programming header 1×6 (TCK/TMS/TDI/TDO/5V/GND) — the primary CPLD programming path (external dongle, as Rev 1)
- [ ] Debug headers: 2× 2×15 THT mirroring the gusmanb level-shifter pinout (pull channel map + gender/orientation from the project KiCad; leave clearance for two analyzers side by side or ribbon out); header 1 = D0–D15 + ~AS/~UDS/~LDS/R/~W/~DTACK + ~RESET/~HALT (23 — spare-channel candidates: audio line clock or paddle DUMP), header 2 = A1–A23 + SYSCLK (via a buffered/series-R tap — do not stub the raw clock net); 5V-ref pins from the rail, 3V3 NC, triggers NC/pads

### Power & decoupling
- [ ] 5.5mm x 2.1mm center-positive barrel jack + inline switch (replaces rev-1 USB-C); silkscreen "5V regulated 2.5A+ center positive" at the jack; jack/switch/traces rated ≥2 A; bulk 220–470 µF at entry + 10 µF per CPLD/FIFO cluster; consider reverse-polarity protection (P-FET ideal-diode style — a series Schottky costs ~0.3–0.4 V the DS1233-5 trip budget can't afford)
- [ ] decoupling cap on every +5V/GND pair, especially the CPLDs

### Pull-ups
- [ ] 4.7K on HALT; any CPU lines that may float or lead
- [ ] ~AS, ~UDS, ~LDS, R/~W (and ~DTACK if ever tri-stated) — strobes must idle deasserted while the CPU is tri-stated (~RESET+~HALT bus-master mode via the debug headers), else GLUE sees phantom cycles; also cleans up LA captures
- [ ] PS/2 CLK & DATA — **both ports**: keyboard on GLUE, mouse on PORTS
- [ ] ~IPL2:0 and ~VPA, 10 K each — GLUE drives them push-pull, but every GLUE→CPU control input floats during GLUE JTAG/ISP or with a blank GLUE, and a floating IPL reading 000 is a level-7 interrupt, which is non-maskable; pulled, the idle reads 111 / VPA deasserted and a half-programmed board fails quiet.  Same ISP-float reasoning as the ~BR/~BGACK pulls (ENGINE side)
- [ ] the IRQ nets into GLUE's priority encoder — nVSYNC (from TIMING), ~DUART_IRQ, ~PORTS_IRQ and **~ENGINE_IRQ** (inert until ENGINE stops tying it high, so without a pull-up it is exactly the kind of net that floats into a phantom level-3)
- [ ] joystick switch lines (bussed resistor networks to +5 V) — except DE-9 pins 5/9 on the paddle port: weak 1 MΩ discretes there, since the paddle pot parallels the pull-up (still fine for reading SMS button 2, just slow edges)
- [ ] I²C SCL/SDA to 5 V
- [ ] JTAG lines
- [ ] any inter-IC signal that could stall or float

### Signal integrity & analog
- [ ] SYSCLK into a GCLK on every CPLD (especially GLUE)
- [ ] source termination (33–100 Ω series at driver output) on every 7200 /RE and /W line, all three streams (Rev 1: unterminated bodge wires rang and doubled reads → image creep; 10 pF-to-GND was the stopgap); no q8_toggle in Rev 2
- [x] no bus transceivers: the 7200 data pins sit directly on D[15:0]; loading accepted (2026-08 electrical review conclusion, re-affirmed 2026-08-31)
- [x] redesign the VGA analog to be robust — done: binary-weighted ladder + shunt, values/BOM/layout in the griffin.yml VGA interfaces entry
- [x] audio output analog stage — designed: CD4049 MSB inverters (two's complement samples), R2R + 10k divider + 3.3nF pole + LM358 buffer + class-A pull-down + coupling; full chain and BOM in the griffin.yml audio interfaces entry; place at the jack

### Test & debug access
- [ ] bring every inter-IC signal (GND, +5V, D, A, WRITE_LO/HI, RAM/ROM/IO/ENGINE/PORTS selects, nVPA) to a 2×N test header with the signal silk-screened per pin
- [ ] female header sized for Dr. Guzman's analyzer (try two ganged); lots of ground test-point holes
- [x] spare GLUE pins — pins 24 and 64 released as bodge spares, routed to pads (griffin.yml interfaces)
- [ ] wire all CPLDs (GLUE/ENGINE/PORTS/PIXEL/COMPOSITOR/TIMING) into the JTAG chain
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
- [x] HDL first: bring every CPLD bitfile to a fit with the fitter free to minimize macrocells, then freeze the pinout before routing — done; the `//PIN:` blocks are marked FROZEN (ENGINE keeps the Rev 1 assignment)
- [ ] hub-and-spoke: bus across from the CPU, peripherals above/below with vertical taps; connectors to the rear
- [x] more inter-CPLD signals — decided: ENGINE_ACTIVE/WAITING → GLUE, CF INTRQ/IORDY → GLUE, spare strobe + PORTS headers; general spare bus rejected (griffin.log 2026-08-30)
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
* **ROM:** 1–2× M29F160FB (16 Mbit 5 V NOR, 1M×16, 55 ns, TSOP48), soldered — grows the Rev 2 SST39SF040 pair to make room for a Linux kernel + erofs root in flash, which 1 MB cannot hold.  512K×8 is the ceiling for 5 V flash in DIP (A18 is the top address pin of the 32-pin JEDEC pinout), so going past 1 MB is exactly what forces the move to TSOP and hence to an SMT respin.  Still in production (Alliance Memory second source; DigiKey/Mouser/TME stock).  FB (bottom boot) preferred: the 16/8/8 KB boot sectors sit at the vector/bootloader end while the erofs image lives in the uniform 64 KB sectors above (FT differs only in sector order).  ×16 mode: BYTE# tied high, flash A0–A19 ← CPU A1–A20, D0–D15 direct — one chip puts 2 MB on the full 16-bit bus (vs 1 MB from the SST pair); an optional second chip selected by A21 makes 4 MB.  Flash window stays at 0x800000–0xBFFFFF as in Rev 2.  55 ns may allow zero-wait ROM at 14 MHz.  Initial flash and recovery via the on-board RP2350B bridge's UF2 drive (see below), which bus-masters JEDEC cycles into the flash window with the CPU held off — this is what buys back the recovery path Rev 2 gets for free from its sockets.  (Offline fallback: XGecu T48 + ADP_F48_EX-1 TSOP48 adapter)  The CPU self-reflashes in-circuit via the same GLUE ~ROM_WE / CONFIG.FLASH_WE_EN mechanism carried forward from Rev 2, except that a single x16 part takes JEDEC command sequences at word addresses 0x555/0x2AA directly rather than Rev 2's byte-duplicated-into-both-lanes form; flasher still runs from RAM (not read-while-write).  RESET# to the ~RESET net.
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
* **Audio:** as Rev 2 — the FIFO pair fed only by ENGINE DMA and popped by PORTS at the TIMING tick rate; no dedicated audio CPLD and no programmable divider (griffin.log 2026-07-28).
* **Interrupts (autovector):** VIDEO 6, DUART 5, PS/2 keyboard 4, ENGINE 3, PORTS 2 (mouse).

## Board changes

*(KiCad watch-list — footprints, pinouts, nets, placement, SI.  Function lives in Rev 3 components above; link, don't restate.)*

### Footprints & sockets
- [ ] ROM: 1–2× M29F160FB TSOP48, **soldered** (image-zero via the RP2350B bridge; bench T48 + ADP_F48_EX-1 as fallback); route ~ROM_WE (carried over from Rev 2), /OE←nR_W, /CE←nROM_SELECT (2nd chip select by A21), BYTE#→+5V, RESET#→~RESET net
- [ ] RAM: 1× AS6C6416 TSOP-II; /UB←nUDS, /LB←nLDS, /OE←nR_W
- [ ] Audio: ~W from ENGINE's AUDIO_FIFO_W strobe, ~R/~RS from PORTS, ~EF to PORTS
- [ ] DUART: XR68C681 DIP-40; A1-A4→RS1-RS4, D0-7 + R/~W direct, ~RESET from ~RESET net, ~IACK tied high (autovectors), 3.6864 MHz crystal on X1/X2 (clearance if ZIF)
- [ ] RP2350B: QFN-80 + QSPI flash + 12 MHz crystal; own USB-C (#1 = board power input, rear); BOOTSEL button + SWD test points; route A1–A23, D0–D15, ~AS/~UDS/~LDS/R/~W, ~DTACK, ~RESET/~HALT/~BOOTSTRAP (47 of 48 GPIOs — one spare)
- [ ] FT2232H: USB-C #2 (data-only, rear); 12 MHz crystal, optional 93LC56 EEPROM; port A JTAG → CPLD chain through series isolation resistors (JTAG backup lives on the debug header, no separate 2×5), port B ↔ DUART Ch A
- [ ] DUART Ch B TTL header (RS232 level-shifter module optional)
- [ ] RTC: DS3231 + coin-cell holder; SCL=OP2, SDA via OP3→2N7000 (drain on SDA) + IP2 readback
- [ ] PS/2: fix footprint + pin mapping — two ports, keyboard to GLUE and mouse to PORTS
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
