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

**This document describes Rev 2**, the board being laid out now.  Rev 1 (the board on the bench) is summarized near the end for the record; Rev 3 and Rev 4 are the sections after it.

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

Logic analyzer: [gusmanb LogicAnalyzer hardware](https://github.com/gusmanb/logicanalyzer/wiki/02---LogicAnalyzer-Hardware) — the debug headers are laid out to mate with its level-shifter board.

# Design philosophy

Try to do something more long-term sustainable that you can pick up and restart more easily  
Find a way to share constants between Verilog, linker.ld, crt0.s, and C++

* Codegen from a master file

How much design file can be in YAML or in Python?  Generate from YAML:

* Constants in Verilog, linker.ld, crt0.s, and C++  
* Check against netlist from KiCAD

## Golden references

How to reduce chance of spin and reduce likelihood of bodge?  Use golden references, everything else flows from them.

* [griffin.yml](griffin.yml) — interfaces between chips, register maps, constants
  * Check the KiCad netlist against it
  * Generate the emulator from it
  * codegen.py emits C/C++, GAS, ld and Verilog headers; a register address exists in exactly one place
* Verilog — uses the generated constants
  * Require fit
  * Dictates the pin layout of each CPLD: the `//PIN:` block at the bottom of each .v is the pinout, and the board routes from it — a copy drifts
* Verify netlist and fit against each other
* Verify electrical behavior of all analog components from the netlist
* Maybe an unstructured English definition managed by AI that states machine capabilities and components?  Verify YAML and definition and netlist and fit all against each other?

**HDL first:** bring every CPLD bitfile to a fit with the fitter free to minimize macrocells, then freeze the pinout before routing.  Done for Rev 2; the `//PIN:` blocks are marked FROZEN (ENGINE keeps the Rev 1 assignment).  A netlist change that makes the fitter want a different placement on GLUE or PORTS is a respin, not a re-fit.

# Case and form factor

~~Maybe target a standard case form factor (e.g. Micro-ATX or Mini-ITX) - use your existing old case?~~

**Nah.**  

~~**Use USB\_C and print a case.**~~

**3D-printed case.**  Power comes in at a panel-mount barrel jack with an inline switch (round cutout) and is harnessed to the board's power connector — see Power.  Connectors to the rear.

## Rear panel

* Serial: DB-25 female (channel A, console, wired DCE) and DB-25 male (channel B, modem, wired DTE)
* Keyboard and mouse: matching pair of full-size DIN-5 (AT-style); PS/2 electrically
* Video: DE-15 VGA
* Game ports: 2× DE-9 male, Atari convention; paddles on port 1
* Audio: RCA stereo pair (line out, fixed level) + 3.5 mm stereo headphone jack; volume knob governs headphones and the internal speaker; plugging in headphones mutes the speaker
* CF card slot
* Power: barrel jack + inline switch; reset button
* Cosmetics: diffused LEDs (power, DEBUG), real toggle/momentary switches, plain D-sub hoods

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

## Rev 2 software plan

Clean everything up for Rev 2, get as much tested as possible

* SW improvements
  * graphics routines, take "blit" out of splash.cpp
  * factor out font - should be selectable by enum
* Booter & apps
  * Have a shell in the ROM
  * Image viewer app
  * Movie player app with audio - microham mode
  * BASIC (finish up your basic.cpp)

# Components

*(Parts and their function — the Rev 2 spec.  Footprints, nets, placement, and layout gotchas live in Board changes, not here.  Registers, bitfields and the full address map are in [griffin.yml](griffin.yml).)*

## Power

The motherboard is **+5 V only**.  No other rail exists on the board or may be added; RS-232 levels come from the MAX232 charge pumps, and anything external that needs another voltage brings its own supply.

* Board connector: keyed 6-position AMP MATE-N-LOK (the Apple II supply connector).  +5 V and GND on the positions Apple used for them, every other position not connected, so a mis-mated Apple II supply does no harm.  **Verify the Apple II pinout before silk.**  Never a bare pin header.  Silkscreen at the connector: "5V ONLY — regulated 2.5A+".
* Panel: 5.5mm x 2.1mm center-positive barrel jack + inline switch, harnessed to the MATE-N-LOK.  Regulated 5V ≥2.5A adapter.  Have adapters, can make/buy more; a CH224K-style PD trigger module with a barrel pigtail turns any PD charger into a compliant supply if ever wanted.
* Budget is ~1.3–1.7 A typ (see rev2-electrical-review.md §8).  Harness and contact drop count against the DS1233-5 trip budget below.
* Why not USB-C: a plain-Rd USB-C sink is only *entitled* to 500 mA without reading the source's Rp advertisement or negotiating PD.

## Power-on reset and user reset button

DS1233-5 reset supervisor.  Datasheet (041002) facts: open-drain RST with internal 5 kΩ (3.75–6.25 k) pull-up, trip point 4.50–4.74 V (typ 4.625), 350 ms stretch, IOL 8 mA.

* The user button hangs on the supervisor's RST pin: the plain (non-D) part senses the low as a pushbutton press, debounces it, and re-stretches the full reset after release, so the button needs no RC of its own.
* RST reaches the system ~RESET net through a Schottky diode (anode on the net, cathode at RST): the supervisor and button pull the net low (asserted level VOL + Vf ≈ 0.5 V, under the 0.8 V TTL VIL), while lows driven on the net — the CPU's 124-clock RESET-instruction pulse, the debug-header master — never reach RST, so they cannot trip the pushbutton detect and be stretched.
* RST also feeds GLUE's nSUPERVISOR_RESET input, the source of ~HALT assertion.
* ~RESET net wire-OR drivers: the diode, the CPU, the debug-header master; 10k pull-up; no RC on the net.
* ~HALT open-drain wire-OR: GLUE drives it low while nSUPERVISOR_RESET is asserted (power-on, brownout, button — and NOT the CPU's own RESET instruction, which GLUE does not echo back); the CPU drives it low on a double bus fault; the debug-header master may hold it with ~RESET for bus mastering.  4.7k pull-up; nothing may drive it high.  GLUE senses ~HALT: while low with GLUE out of reset, DEBUG_OUT blinks at vsync/32 (~1.9 Hz) so a halted CPU is visible.
* Bring-up gate: rail ≥ 4.85 V at the supervisor under full load, else DS1233-10 or trim the supply.
* Firmware never executes the RESET instruction: its pulse still resets GLUE (CONFIG clears, ROM overlay returns) under a still-running CPU.

## System clock

Three oscillators; no CPLD re-drives a clock.

* SYSCLK — 14 MHz can → CPU CLK and pin 83 (GCLK1) of GLUE, ENGINE and PORTS.  Okay to change and reflash bitfiles if the clock is changed; DTACK tables in griffin.yml cover 12 / 14 / 14.318 / 16 MHz.
* Pixel — 25.175 MHz can → TIMING pin 43 (GCLK1, PLCC44) and pin 83 (GCLK1) of PIXEL and COMPOSITOR.  TIMING pin 44 (GCLK2) stays free for an optional second oscillator.
* The DUART runs its own 3.6864 MHz crystal on X1/X2.

## CPU

MC68010 at 14 MHz, zero wait states to RAM; a 68000 is a drop-in.  64-pin DIP.

* Redo board to slot in a 68030 at some later date if desired — that is Rev 4

## RTC

DS3231 (TCXO) on I²C via the DUART's spare OP/IP pins — off the bus, zero GLUE logic (no A17, no chip-select/strobes/wait-states), 5 V so no level translation.

* SCL = OP2 (push-pull is fine; DS3231 doesn't clock-stretch)
* SDA is open-drain via OP3 → 2N7000 pull-down (drain on SDA), IP2 reads it back
* INT#/SQW → IP3 (change-detect) for a 1 Hz/alarm IRQ
* Coin-cell holder
* Chosen over the BQ3285, whose 146818-style multiplexed bus would have cost GLUE A17 + AS/DS strobes + 3 pins

## Address map

* RAM 0x000000–0x3FFFFF, 4 banks selected by A20/A21
  * Populate banks contiguously from bank 1; DTACK responds whether RAM is populated
  * ROM overlaid over RAM bank 1 at reset until CONFIG.ROM_OVERLAY_DISABLE
* **ROM window 0x800000–0xBFFFFF** (decode = A23 & ~A22, two literals); the 1 MB image mirrors 4× through the 4 MB window
* Peripherals under A23 & A22: ENGINE 0xD00000 (GLUE decodes ~ENGINE_SELECT for it), GLUE 0xF00000, CF 0xF40000, DUART 0xF80000, **PORTS 0xFC0000**
* 0xC00000–0xCFFFFF and 0xE00000–0xEFFFFF are undecoded — BERR by timeout
* The audio FIFOs are reached only by ENGINE DMA; there is no CPU-mapped write path

See [griffin.yml](griffin.yml) for the complete peripheral address map.

## ROM

2× SST39SF040-70-4C-PHE (512K×8 5 V NOR flash, DIP-32, socketed) = 1 MB on the 16-bit bus, one per byte lane

* Single 4.5–5.5 V supply for read *and* program/erase (Vpp on-chip), so the TL866II+ programs them in the bare DIP-32 socket
* Probably should revise ROM DTACK to match flash
* Decode, /OE←nR_W and the boot overlay are unchanged from the W27C512 build — same A23 & ~A22 nROM_SELECT
* **The CPU can reflash in circuit.**
  * GLUE drives ~ROM_WE to both WE# pins, asserted only for (A23 & ~A22) & ~R/W & AS & UDS & LDS & CONFIG.FLASH_WE_EN (resets to 0 = write-protected)
  * Demanding both data strobes makes byte writes produce no strobe at all, so nothing can feed a command to one x8 chip and desync the pair
  * Qualifying on the raw region instead of nROM_SELECT keeps overlay-window writes off the flash regardless of the overlay bit
  * JEDEC x8 command bytes are duplicated into both halves of each word (unlock 0xAAAA→0x80AAAA, 0x5555→0x805554); one word write programs both lanes in parallel; DQ7/DQ6 are polled per lane
  * Sector erase is 4 KB per chip = 8 KB of CPU space
  * Flasher runs from RAM (.ramtext) with the overlay off — the parts return status, not instructions, during an embedded operation
* Because they stay socketed, a bricked image is recovered by pulling them and using the bench programmer.  Rev 2 has no bus-master bootstrap path (that arrives with the RP2350B bridge in Rev 3).

## RAM

4 MB as 4 banks × 2 of AS6C4008 (512K×8, DIP-32, socketed, bin parts).

* GLUE drives nRAM_[1-4]_SEL (banks by A20/A21) and WRITE_LO/WRITE_HI byte write strobes; /OE←nR_W
* Zero wait states at 14 MHz
* Banks are fully populated; no code manages partial population.  (Contiguous partial population from bank 1 still works electrically.)

## GLUE logic

Dedicated ATF1508 CPLD for:

* Address decode: nRAM_[1-4]_SEL, nROM_SELECT, CF incl CS0/CS1 + AS-gated IORD/IOWR, ~DUART_SELECT, ~PORTS_SELECT, ~ENGINE_SELECT (a real net, GLUE pin 39 → ENGINE pin 84)
* DTACK / wait-state generation for everything including PORTS and CF (CF IORDY low extends the cycle); BERR by timeout for the undecoded regions
* WRITE_LO / WRITE_HI byte write strobes from ~UDS, ~LDS and R/~W
* ~ROM_WE for in-circuit flash programming (word writes only, gated by CONFIG.FLASH_WE_EN)
* Boot ROM overlay + the CONFIG register
* Autovectors: GLUE asserts ~VPA instead of ~DTACK during IACK cycles (FC=111)
* Priority encoder for the IRQ nets (see Interrupts); ~ENGINE_IRQ is a level-3 input, ~PORTS_IRQ a level-2 input; nVSYNC from TIMING is edge-latched here with W1C ack (VSYNC_STATUS / VSYNC_CLEAR) because TIMING has no bus to host one
* PS/2 **keyboard** frame engine: assembles a full 11-bit PS/2 frame in hardware and raises one level-4 IRQ per byte on RX (byte in PS2\_RX\_DATA, parity/framing flags in PS2\_STATUS for firmware to check); on TX shifts a host-to-device frame out on the device clock (the PS2\_TX\_DATA write presents the start bit and releases CLK; ACK sampled into PS2\_STATUS.TX\_ACK; TX\_DONE IRQ on completion).  Firmware-computed odd parity is carried in PS2\_TX\_DATA address bit 1 (0x09 = parity 0, 0x0B = parity 1).  Half-duplex, one shared shifter.  The mouse is a second copy of this engine over in PORTS.
* Reset and halt: nSUPERVISOR_RESET in from the DS1233; ~HALT asserted while it is (see Power-on reset)
* DEBUG\_OUT
  * Sets or clears debug LED and test point output
  * Pre-DUART boot "alive" blink, video-ISR heartbeat, TX-timeout/panic blink, and the ~1.9 Hz halted-CPU blink
* Serial and the system tick are the 68681 DUART, not GLUE
* Pinout frozen in the `//PIN:` block at the bottom of glue.v (pin-full: one spare I/O); pins 24 and 64 released as bodge spares, routed to pads.  The design needs `xor_synthesis` to fit and drops `-strategy debug`.  Fit history in griffin.log.
* Registers: see [griffin.yml](griffin.yml).

## Video — VGA 640×480@60, 12-bit R4G4B4, display-list driven ("super-engine")

[Griffin Video Mode Throughput](https://docs.google.com/spreadsheets/d/1jpam0LNxlgqLVfV4WW1wBMNDqXu4QpefYhichfac1WE/edit?usp=sharing)

VGA only; NTSC/composite dropped.  The path is four chips: **ENGINE → {PIXELS, VIDCMD} FIFOs → PIXEL (+TIMING) → COMPOSITOR → resistor DAC**.  Semantics live in vidcmd-spec.md; the executable model + budget checker is super-engine/; shared numbers and board support parts are in griffin.yml `constants:` and `interfaces:`.

* **ENGINE** (engine.v, Rev-1 pinout): walks 4-word descriptors from the top 64K of RAM.  Each descriptor says: wait for hblank or don't, read N words from this address, and pulse this FIFO's write strobe (PIXELS / VIDCMD / AUDIO, one-hot) — or strobe nothing, which just burns bus time to place a later deposit.  The last one raises the level-3 IRQ and stops; the CPU's vsync ISR re-arms with the other buffer.  Rule learned twice: *any* RAM a list points at is frame-owned — double-buffer the lists and their data.
* **PIXEL** (pixel.v): unpacks the PIXELS FIFO — pure pixel bits, no in-band header — in 1bpp (40 words/line) or 2-bits-per-clock micro-HAM (80 words/line), through 12-bit palette/held registers, out as R4G4B4.  On FIFO underrun it re-shifts the last word rather than erroring, which turns short fills into a compression feature: one 0x0000 word paints background to end-of-line, or end-of-frame.
* **TIMING** (timing.v, ATF1504 PLCC44, testbench timing_tb.v): raster counters, syncs, /RS at vsync, the three event strobes PIXEL consumes, and the board's other time bases — HBLANK to ENGINE, nVSYNC_IRQ to GLUE, and the PADDLE_TICK / AUDIO_TICK square waves (toggle per line; PORTS counts falling edges: 15.734 kHz) — plus four pins reserved for DUART OP4..OP7 as a future mode/rate selector.  Split out because PIXEL+TIMING together do not fit one ATF1508 (pixel_combined.v is the kept negative result).  Every stage's fixed pipeline delay is cancelled here by firing comparators early: the lead constants (COMPOSITOR_LEAD 2, DAC_LEAD 4, …) are *derived* in griffin.yml, not tuned on hardware.
* **COMPOSITOR** (compositor.v; its iverilog testbench is the timing-semantics spec): executes the VIDCMD instruction stream — RUN spans (passthrough, two held colors, or eight saturated RUN_COLOR literals) and SET, which writes its own held colors or forwards a 12-bit value into PIXEL's registers over a valid/target/commit handshake.  Per-line palettes, pixel-exact mid-line color changes, sprites-as-spans, and full-screen RLE images are all just authored stream content.  When the FIFO runs dry at a record boundary the current span continues to end of line — so lines with nothing to say cost zero words.
* **No CPU bus on PIXEL, TIMING, or COMPOSITOR.**  Every register they have is reached only by SET instructions the CPU authors into RAM and ENGINE DMAs into the VIDCMD FIFO.  vsync IRQ (level 6) comes from TIMING; ENGINE's level-3 IRQ is the list-completion handshake.
* **FIFOs:** all three streams use 2× IDT7200 (256×9) — PIXELS byte-interleaved shared-Q pair (/RE from PIXEL), VIDCMD ganged 16-bit (/RE from COMPOSITOR), AUDIO one per channel (/RE from PORTS).  Each pair's /W is one ENGINE deposit strobe; data pins sit directly on D[15:0] (no transceivers — loading accepted, 2026-08 electrical review).  Series source termination at the driver end on every /RE and /W line.  XI/XO cascade is an unpopulated-footprint option.
* **DAC:** binary-weighted resistor ladders from COMPOSITOR's R[3:0], G[3:0], B[3:0] through 74AC541 buffers; ladders and shunts at the DE-15, not at the buffers.  Syncs: buffer → 100R series → DE-15 pins 13/14, no termination.  DE-15: R=1, G=2, B=3, HS=13, VS=14; 5,6,7,8,10 + shell → GND.  Values and BOM in the griffin.yml VGA interfaces entry.

## Compact Flash interface

* 16-bit True IDE PIO on D[15:0]
* CS0/CS1 from GLUE; IORD/IOWR gated by AS in GLUE (R/~W alone leaves AS gone at IOWR rise → junk data)
* GLUE manages DTACK; IORDY low extends the current cycle
* INTRQ → GLUE: readable in CF_PINS, optionally raises the level-1 IRQ
* DMACK to +5
* Registers: see [griffin.yml](griffin.yml).

## Serial — XR68C681 DUART

* **Channel A = console**, 115200 8N1.  crt0 brings the DUART up early and all boot/exception/panic prints go through it (stack-free putchar, TXRDY-poll with timeout to LED blink); pre-DUART failures are LED-blink only.
  * Real RS-232 on a DB-25 female wired DCE, so a terminal (DTE) uses a straight cable.  Pin 2 → receiver → RXDA; TXDA → driver → pin 3; terminal RTS (pin 4) → receiver → IP0 (~CTS_A); OP0 (~RTS_A) → driver → pin 5 (terminal CTS); strap DSR (6) and DCD (8) to the terminal's DTR (20) at the connector; pin 7 signal ground.
* **Channel B = modem / second serial.**  Real RS-232 on a DB-25 male wired DTE — straight cable to a Hayes-style modem, null modem to a computer peer.  TXDB → driver → pin 2; pin 3 → receiver → RXDB; OP1 (~RTS_B) → driver → pin 4; pin 5 → receiver → IP1 (~CTS_B); pin 8 (DCD) → 75189 → IP4; pin 22 (RI) → 75189 → IP5; DTR (20) strapped to DSR (6) at the connector (modem set &D0; hang-up via +++ATH; a GLUE bodge spare can drive DTR later if ever wanted); pin 7 signal ground.  DCD/RI are poll-only: the 68681 change-of-state IRQ covers only IP0–IP3.
* **Level stage:** one MAX232 per channel (2 drivers + 2 receivers = TXD/RXD + RTS/CTS; 1 µF charge-pump caps); ch B's DCD and RI through a 75189 quad receiver (2 of 4 sections, unused inputs grounded).  All stages invert, which lands every polarity correctly.  Deliberately not routed through USB.
* **TTL bench headers remain.**  The DUART TX pins drive header and MAX232 driver inputs in parallel (no conflict); each receiver output (RXD_A, CTS_A, RXD_B, CTS_B) reaches the DUART through a 2-pin series jumper — jumper in = RS-232 port live, jumper out = the TTL header owns the line.  Four jumpers, silk-labeled.
* **C/T = 100 Hz systick** (level 5) + a configurable timer ISR.
* OP/IP allocation (table in griffin.yml): RTS/CTS on both channels, the DS3231 I²C (OP2/OP3/IP2/IP3), DCD_B/RI_B on IP4/IP5, OP4..OP7 → TIMING pins 20/21/24/25 as a reserved mode/rate selector (no logic yet; OPR reset state 1111 must remain today's behaviour).
* Bus: ~CS from GLUE (nDUART_SELECT), DTACKN back to GLUE, ~IACK tied high (autovectors), RESET directly on the system ~RESET net.  DIP-40 native.

## PS/2 keyboard and mouse

* Keyboard: the GLUE frame engine (above) — one level-4 IRQ per byte, TX on the device clock, parity in the write address
* Mouse: an identical engine in PORTS at the same register offsets, so firmware's `ps2.cpp` is one base-parameterized driver serving both; `static_assert`s prove the two maps are offset-identical
* CLK/DATA are open-drain bidirectional with external pull-ups, both ports
* Connectors: matching pair of full-size DIN-5 (AT-style) on the rear panel.  Footprint + pin mapping still to land in KiCad

## PORTS — the fourth CPLD

The PS/2 mouse gets its own CPLD, PORTS, together with the joysticks, the paddle counters and the audio FIFO pop: two PS/2 frame engines do not share one ATF1508, so the keyboard stays in GLUE and the mouse leaves; reading the DE-9s and counting the paddle ramps costs almost nothing in a CPLD that already exists, which retires the 74HCT245 / 74HC590 chips and the DUART OP4/OP5 dump dance; and a programmable audio divider is not worth a chip when a line-rate tick from the timing chip gives the sample rate the design already assumed.  Rev 2 is therefore GLUE, ENGINE, PORTS, PIXEL and COMPOSITOR on ATF1508AS PLCC84 plus TIMING on an ATF1504AS PLCC44, all with frozen pinouts.  Headroom: ENGINE has the only real logic reserve and spare I/O (a generalized COPY-FIFO or a second DMA channel goes there); PORTS has spare pins but little logic, so a small connector-facing addition goes there and anything larger wants ENGINE.  Fit measurements: griffin.log (2026-07-28, 2026-07-30); the experiment that chose the feature set is pcbv2-ports-design.md.

* At 0xFC0000: the PS/2 **mouse** frame engine, both **joystick** ports read as bytes, both **paddle** counters + the dump control, and the **audio FIFO pop** strobe, ~RS control and ~EF status
* Carries no bus timing of its own: GLUE hands it a cycle-qualified ~PORTS_SELECT and answers DTACK, exactly as for CF
* Time bases are two nets from TIMING — **PADDLE_TICK and AUDIO_TICK**, square waves toggling once per line, synchronized on arrival and counted on falling edges (15.734 kHz events for both).  No prescaler, no programmable divider, anywhere.
* ~PORTS_IRQ (level 2) — mouse only
* cpld/ports/ports.v takes its register decode from the generated griffin.yml defines; pinout frozen in its `//PIN:` block; does not use `xor_synthesis` (measured to make no difference)
* Registers: see [griffin.yml](griffin.yml).

## Joysticks and paddles

* 2× DE-9, Atari 2600-style, switch lines **straight into PORTS**; read back as two byte registers (JOYSTICK_PORT_1/2), bits active-low (0 = switch closed)
* U/D/L/R on DE-9 pins 1–4, fire on pin 6; pins 5/9 also wired (Sega Master System pad button 2 works, pads still manufactured), leaving spare bits per lane
* Pull-ups on every switch line, +5 V on pin 7 via polyfuse, GND pin 8; ESD boundary is series R + clamps at the connectors
* Polled at 60 Hz in the vsync ISR — no IRQ, no CPLD state machine; registers defined in griffin.yml so the emulator can map them to keys
* **Paddles — port 1, 2 paddles.**  A 2600 paddle is a 1 MΩ pot from +5 V (pin 7) to pin 5/9 with fire on pins 3/4, so the connector wiring above already covers it.
  * Per paddle: ~10 nF cap to GND + 2N7000 drain FET on the pot line; both FET gates come off one PADDLE_CONTROL.DUMP bit
  * PORTS runs an 8-bit saturating upcounter per paddle at the full VGA line rate — exactly the 2600's method: count while the RC ramp holds the pin below the input threshold, freeze at the crossing; t ≈ 0.69·RC, size C for ≈255-line ≈8.1 ms full scale
  * CPU cost per frame is two byte reads and one byte write; immune to DMA/IRQ jitter
  * Expect software calibration (input threshold varies part to part, and the pot parallels the pin-5/9 pull-up).  Paddle-vs-stick is a per-port software mode, as on the real 2600.

## Audio

Stereo 8-bit sampled, ~15.7 kS/s, DMA-fed.

* **FIFO:** 2× 7200 (256×9; 256 pairs = 16 ms of buffer, cascadable via XI/XO if depth is ever wanted), one per channel; L = D[15:8], R = D[7:0] straight to the FIFOs.
  * ~W is ENGINE's AUDIO_FIFO_W descriptor strobe: the display list deposits sample words by DMA; there is no CPU-mapped write path
  * **PORTS** generates ~R once per AUDIO_TICK event from TIMING and drives both ~RS (AUDIO_CONTROL.RESET, held from power-on until released; also the flush); ~EF goes to PORTS as pollable EMPTY status; ~HF unconnected
  * No audio interrupt — the level is dead-reckoned and a missed frame is detected from the vsync latch; no mono packing
  * Samples are **two's complement** (0x00 = silence): CD4049 inverters flip each channel's Q7 into the unsigned ladders.  The inverter must be CMOS-output; a TTL high would shrink the MSB weight ~25% at the zero crossing.
* **Output stage, per channel:** 7200 Q[7:0] → CD4049 on Q7 → 4610X-R2R-103LF 10k ladder → 10k to GND (with the ladder's 10k Thevenin this IS the 2:1 divider: 0–2.5 V, 5k source) → 3.3 nF to GND (single pole ~9.7 kHz) → LM358 half as unity buffer → 4.7k to GND at the output (class-A bias against crossover) → 100 µF AC coupling → the LINE node.  Three consumers hang on LINE, so the level there is knob-independent:
  * **Line out:** LINE → 100R → RCA jack (L and R).  Fixed level, always on; 2.5 Vpp max ≈ 0.88 Vrms at full scale.
  * **Volume pot:** dual-gang 10k audio taper, element top to LINE, bottom to GND (the pot IS the coupling cap's DC return), wiper out.  A fixed 10k across LINE, so the knob does not move the line-out level.
  * **Headphones:** wiper → 10k / 1k pad (~11:1 — an LM386 cannot go below gain 20, and 0.88 Vrms × 20 would clip a 5 V rail) → LM386 (pins 1/8 open, gain 20) → 10R + 47 nF Zobel → 250 µF → switched 3.5 mm stereo jack.  ~1.6 Vrms max into 32 Ω (~80 mW); the knob sets it.
* **Internal speaker** (replaces the Rev 1 buzzer): the jack's normally-closed tip and ring switch contacts carry the two headphone-amp outputs, each through 47k into a mono sum at the speaker LM386 input, 1k to GND (a ~25:1 pad: ~65 mV at the amp → ~1.3 Vrms into the speaker, ~200 mW, no clipping at any knob position; the 1k also holds the input quiet when the contacts open).  LM386 gain 20 → 10R + 47 nF Zobel → 250 µF → 8 Ω speaker.  Inserting a plug opens both contacts and mutes the speaker.
* **Three LM386s total** — L and R headphone drivers plus the speaker amp.  Behaviour: RCA line out ignores the knob; the knob governs headphones and speaker together; headphones mute the speaker.  Software mute is AUDIO_CONTROL.ENABLE (DACs freeze on a DC level) and silences all three.
* Placement: ladder, divider, filter and buffer at the RCAs; headphone amps at the 3.5 mm jack; pot and speaker are panel/case parts.  Full chain and BOM in the griffin.yml audio interfaces entry.

## Interrupts (autovector)

* 6: vsync — TIMING's dedicated nVSYNC_IRQ output (pin 31) to GLUE pin 65, polarity fixed regardless of raster mode, edge-latched in GLUE, W1C via VSYNC_CLEAR
* 5: DUART (systick + serial)
* 4: PS/2 keyboard (GLUE)
* 3: ENGINE (list completion)
* 2: PORTS (mouse only)
* 1: CF INTRQ (optional, GLUE)

## Debug and programming access

* **JTAG programming header 1×6** (TCK/TMS/TDI/TDO + 5 V + GND) — the primary CPLD programming path, external dongle.  All six CPLDs (GLUE/ENGINE/PORTS/PIXEL/COMPOSITOR/TIMING) in the chain; chain order in cpld/Makefile still describes the Rev 1 two-TAP chain until the board is routed.
* **Debug headers — LA access + "PCB design failed" backup:** two 2×15 0.1" headers laid out to **mirror the LogicAnalyzer level-shifter board's own 2×15 pinout** (24 channels + GND + 3V3 + 5V-reference + 2 external-trigger pins each), so the two on-hand 5 V-tolerant analyzers plug straight on and daisy-chain via their own 3-pin chain connectors into one synchronized 48-channel capture.  Exact channel-to-pin mapping, mating gender, and orientation come from the gusmanb KiCad files at schematic time — the wiki shows it only as a diagram.
  * Channel plan (47 bus/control signals + SYSCLK = 48, an exact fit): **header 1 "what happened"** = D0–D15, ~AS, ~UDS, ~LDS, R/~W, ~DTACK, ~RESET, ~HALT (exactly 23; spare-channel candidates: audio line clock or paddle DUMP); **header 2 "where"** = A1–A23 + SYSCLK (24, via a buffered/series-R tap — do not stub the raw clock net)
  * 5V-reference pins fed from the Griffin rail (analyzer's onboard jumper removed → external reference); 3V3 pins NC; trigger pins NC/test pads
  * Roles: (1) *Observe:* whole-bus captures as above.  (2) *Backup bus-master:* every signal a master needs is on these same two headers — hold ~RESET+~HALT (68000 tri-states A/D/strobes; GLUE still does its normal decode, so the flash sees ordinary write cycles).  Bus-cycle mechanics for whoever masters: bit-banged/PIO read_word/write_word (honor ~DTACK or run conservatively slow).
* **DEBUG LED** via a separate NPN driver (2N3904 / SOT-23 MMBT3904; base via 1–10 K to DEBUG_OUT) — boot codes / video-ISR heartbeat / double-fault / halted-CPU blink
* Console UART TTL header and the channel B TTL header, behind the isolation jumpers (see Serial)
* Test header: every inter-IC signal (GND, +5V, D, A, WRITE_LO/HI, RAM/ROM/IO/ENGINE/PORTS selects, nVPA) on a 2×N header with the signal silk-screened per pin; GLUE spare pins 24 and 64 on pads; lots of ground test-point holes

# Board changes

*(KiCad watch-list — footprints, pinouts, nets, placement, SI.  Function lives in Components above; link, don't restate.)*

**CPLD pin numbers are not in this document.**  The authoritative Rev 2 pinout for each part is the `//PIN:` block at the bottom of its Verilog — cpld/glue/glue.v, cpld/ports/ports.v, cpld/compositor/compositor.v, cpld/pixel/pixel.v and cpld/pixel/timing.v (all **FROZEN**), and cpld/engine/engine.v (carried from the Rev 1 hand assignment).  Route from those files, not from a copy — a copy drifts.  A netlist change that makes the fitter want a different placement on GLUE or PORTS is a respin, not a re-fit.

### Footprints & sockets
- [ ] ROM: 2× SST39SF040 DIP-32 in sockets (one per byte lane, 1 MB ×16 — note DIP-32, **not** the Rev 1 DIP-28 W27C512 footprint); /OE←nR_W, /CE←nROM_SELECT, WE#←~ROM_WE (new net, both chips in parallel); initial images via the bench programmer, thereafter self-reflash
- [ ] RAM: 8× DIP-32 sockets, 4 banks × 2 of AS6C4008 (populate contiguously from bank 1); /OE←nR_W, WRITE_LO/WRITE_HI byte strobes + nRAM_[1-4]_SEL from GLUE
- [ ] Audio: 2× 7200 FIFO + 2× R2R DAC on the board; ~W from ENGINE's AUDIO_FIFO_W strobe pin, **~R and ~RS from PORTS, ~EF to PORTS**, ~HF unconnected
- [ ] Audio connectors: 2× RCA (line out) + 1× switched 3.5 mm stereo jack (headphones; NC contacts feed the speaker sum); RCA retainer feet — partial holes?; dual 10kA pot + 8 Ω speaker are panel parts, bring them to a header
- [ ] Video FIFOs: 2× 7200 on the board natively (retires the piggyback bodge of video-fifo-wiring.md); termination in SI section below
- [ ] VGA: DE-15 connector; remove the composite/NTSC jack and any NTSC clock provisions — Rev 2 is VGA-only
- [ ] DUART: XR68C681 DIP-40 native (retires the DIP-carrier bodge); A1-A4→RS1-RS4, D0-7 + R/~W direct, ~RESET from ~RESET net, ~IACK tied high (autovectors), 3.6864 MHz crystal on X1/X2 (clearance if ZIF)
- [ ] Serial: 2× MAX232 (bin) + 75189 (bin) level stages; DB-25F console wired DCE (DSR+DCD strapped to DTR), DB-25M modem port wired DTE (DTR–DSR strap; DCD pin 8 → IP4, RI pin 22 → IP5); TTL bench headers retained behind 4 isolation jumpers on the receiver outputs
- [ ] RTC: DS3231 + coin-cell holder; SCL=OP2, SDA via OP3→2N7000 (drain on SDA) + IP2 readback
- [ ] PS/2: **two ports**, full-size DIN-5 pair; fix footprint + pin mapping (Rev 1 mini-DIN-6 was wrong); keyboard CLK/DATA to GLUE, mouse CLK/DATA to PORTS
- [ ] PORTS: ATF1508AS PLCC84 socket + JTAG chain; nets ~PORTS_SELECT / ~PORTS_IRQ / A4-A1 / ~LDS / R~W / D7-D0 to GLUE and the bus, **PADDLE_TICK (TIMING 17 → PORTS 44) and AUDIO_TICK (TIMING 18 → PORTS 45)** as its only time bases, ~R, ~RS and ~EF to the 7200 pair, mouse PS/2, both DE-9s, PADDLE_DUMP to the FET gates — pin numbers from the frozen `//PIN:` block in cpld/ports/ports.v
- [ ] TIMING fan-out (pins from the `//PIN:` block in cpld/pixel/timing.v): HBLANK (pin 19) → ENGINE pin 2; nVSYNC_IRQ (pin 31) → GLUE pin 65 (VGA_VSYNC no longer tee'd); VGA_HSYNC/VGA_VSYNC only to the 74AC541s; DUART OP4/OP5/OP6/OP7 → TIMING pins 20/21/24/25 (reserved, no logic yet — route them anyway); leave GCLK2 (pin 44) free with an optional oscillator footprint; the remaining TIMING spares are bitfile-usable later only if routed to reachable copper (header or test points)
- [ ] ~ENGINE_IRQ: ENGINE pin 5 → GLUE pin 68; needs a pull-up like the other IRQ nets (see Pull-ups)
- [ ] No 74155 and no direct-bus region in Rev 2 (deleted with the CPU audio path); ~ENGINE_SELECT *is* a net (GLUE pin 39 → ENGINE pin 84); CF CS1 stays routed from GLUE
- [ ] Joysticks: 2× DE-9 male PCB-mount, switch lines **straight into PORTS** — no '245s; bussed pull-up networks, polyfuse on pin-7 +5V, optional series R + clamps between connector and the CPLD
- [ ] Paddles (port 1 only): 2× {~10 nF film cap to GND + 2N7000 drain, gate←PADDLE_DUMP from PORTS} on DE-9 pins 5/9, pin 5/9 also to PORTS as the count-enable senses — no '590s; all DNP-able, a sticks-only build omits the caps/FETs
- [ ] everything THT / socketed where practical — Rev 2 stays bodgeable by intent
- [ ] JTAG programming header 1×6 (TCK/TMS/TDI/TDO/5V/GND) — the primary CPLD programming path (external dongle)
- [ ] Debug headers: 2× 2×15 THT mirroring the gusmanb level-shifter pinout (pull channel map + gender/orientation from the project KiCad; leave clearance for two analyzers side by side or ribbon out); header 1 = D0–D15 + ~AS/~UDS/~LDS/R/~W/~DTACK + ~RESET/~HALT (23 — spare-channel candidates: audio line clock or paddle DUMP), header 2 = A1–A23 + SYSCLK (via a buffered/series-R tap — do not stub the raw clock net); 5V-ref pins from the rail, 3V3 NC, triggers NC/pads

### Power & decoupling
- [ ] Keyed 6-position AMP MATE-N-LOK power connector on the board, +5 V/GND only on Apple's positions, the rest N/C — verify the Apple II pinout before silk; silkscreen "5V ONLY — regulated 2.5A+"; connector/traces rated ≥2 A
- [ ] Panel harness: 5.5mm x 2.1mm center-positive barrel jack + inline switch to the MATE-N-LOK (replaces rev-1 USB-C); the harness drop is inside the DS1233-5 trip budget
- [ ] bulk 220–470 µF at entry + 10 µF per CPLD/FIFO cluster; consider reverse-polarity protection (P-FET ideal-diode style — a series Schottky costs ~0.3–0.4 V the DS1233-5 trip budget can't afford)
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
- [x] audio output analog stage — designed: CD4049 MSB inverters (two's complement samples), R2R + 10k divider + 3.3nF pole + LM358 buffer + class-A pull-down + coupling → LINE; RCA line out; pot; pad + LM386 headphone drivers; NC-contact sum + LM386 speaker amp (three LM386s); full chain and BOM in the griffin.yml audio interfaces entry; DAC/buffer at the RCAs, headphone amps at the jack

### Test & debug access
- [ ] bring every inter-IC signal (GND, +5V, D, A, WRITE_LO/HI, RAM/ROM/IO/ENGINE/PORTS selects, nVPA) to a 2×N test header with the signal silk-screened per pin
- [ ] female header sized for Dr. Guzman's analyzer (try two ganged); lots of ground test-point holes
- [x] spare GLUE pins — pins 24 and 64 released as bodge spares, routed to pads (griffin.yml interfaces)
- [ ] wire all CPLDs (GLUE/ENGINE/PORTS/PIXEL/COMPOSITOR/TIMING) into the JTAG chain
- [x] separate DEBUG LED + NPN driver (2N3904 / SOT-23 MMBT3904; base via 1–10 K to DEBUG_OUT)

### Carryover bug fixes
- [ ] fold every entry of the Rev 1 bodge record (see Rev 1, below) into the schematic natively
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

## Possible new hardware features

Generalized ENGINE — to look into:

* Stream into VIDEO, AUDIO, or COPY FIFOs from RAM, ROM, CF
* Stream out of COPY FIFO to RAM
* Optional block on HF for VIDEO, AUDIO FIFOs
* Fixed size?  Or count for COPY ops?
* Net impact: as bus master ENGINE reaches the audio FIFO / CF through normal GLUE-decoded cycles, so mostly no new nets — the exception is observing the FIFO HF flags it must block on (AUDIO ~HF at minimum); decided with the "more inter-CPLD signals" layout item

# Rev 1

*(The board on the bench.  Superseded by Rev 2 above; kept for the record because every Rev 2 decision was learned here.  Any revelations from rev 1 feed into rev 2; a rev1 branch continues experiments while main is rev 2.)*

## Rev 1 as built

* **Power:** USB-C with switch inline between supply and USB-C port; "two 5.1k pull-downs on both CC pins" — measured 0.84 A draw
* **Reset:** R2/C3/SW1 RC + button only (no supervisor)
* **System clock:** CPUCLK - 16MHz oscillator, okay to change and reflash GLUE bitfile if clock is changed
* **CPU:** 68000P12, upgradeable to 68EC000-20 or 68010@12 or any 68K in 64-DIP format
* **RTC:** none.  Completely forgot from the beginning; full up on pins in GLUE and in IO MCU, so not easy to add, and board space may not be available
* **Address map:** 0x0 through 0xBFFFFF - RAM addressing but only sockets for 4MB
  * 0b0000\_xxxx\_xxxx\_xxxx\_xxxx\_xxx, 0x0X\_XXXX is bank 1  
  * 0b0001\_xxxx\_xxxx\_xxxx\_xxxx\_xxx, 0x1X\_XXXX is bank 2  
  * 0b0010\_xxxx\_xxxx\_xxxx\_xxxx\_xxx, 0x2X\_XXXX  is bank 3  
  * 0b0011\_xxxx\_xxxx\_xxxx\_xxxx\_xxx, 0x3X\_XXXX  is bank 4  
  * Fill them in sequentially; DTACK responds whether RAM is populated, D will be repeating within partially populated, floating/junk for not-populated  
  * ROM overlaid over RAM bank 1 until write to GLUE config register
  * RAM sizing test: write 0xAA55 to 4M - 2, write 0x7733 to 4M - 4, if 4M - 2 == 0xAA55, then 4M; repeat for 3M, 2M, 1M; otherwise assume 256K
* **ROM:** 128K from 2× W27C512 (DIP-28).  Simple bootloader, simple shell, no video config in ROM, load and run from CF, receive and run over serial, maybe even stick BASIC in there.  DTACK follows AS by a clock for 12MHz CPU clock or two clocks if 16MHz or 20MHz CPU.  Filled to 91% by the time Rev 2 was planned.
* **RAM:** initially 256KByte from 2x 128K KM681000BLP-7 SRAM; upgrade to 4M by populating eight 512KByte AS6C4008 parts (x8 is going to be > $60); A20,A21 select between 4 banks; can have incomplete banks but all lower banks must be populated or RAM will be sparse
* **GLUE (rev 1):** receive RESET and assert HALT; address decode ~ROM\_SELECT, ~RAM\_BANK\_{n}\_SELECT, ~IO\_SELECT\_MOSI, ~VIDEO\_SELECT, ~CF\_CS0, ~CF\_CS1, ~AUDIO\_LE, ~ENGINE\_SELECT; invert R/~W to output ~R/W; decode ~UDS/~LDS/R/~W into ~WRITE\_LO/~WRITE\_HI; the PS/2 frame engine (carried into Rev 2 unchanged); autovector ~VPA (bodge wire to GLUE 75); interrupt levels 7: VIDEO, 6: ENGINE, 5: IO; DTACK counted off per region from YAML, OR'd with ENGINE\_DTACK / IO\_DTACK, AND'd with VIDEO\_STALL; BERR after 8 cycles (256 for IO\_DTACK); DEBUG\_OUT
* **VIDEO (rev 1):** NTSC and VGA pixel and timing generation on the second ATF1508.  16-bit shift register clocks out 1 bit, expands to R3G3B2 through an internal pair of palette registers; counts off hsync and vsync and raises the exit-VBLANK interrupt through GLUE; all config registers default to 0 (video disabled); changes happen in hblank or vblank because the CPU is in a tight pixel loop during visible lines.  Snooped bus user data (FC2:FC0 == 101) in "slow palette" mode expects 16 bits of pixel data repeated through visible pixels.  In-band 4-byte header per line; ENGINE streamed pages from RAM by SOURCE\_PAGE.  The 7200 FIFOs were a piggyback bodge (video-fifo-wiring.md).
* **Compact Flash (rev 1):** 8-bit access only to ease routing, D0-D7 so only odd addresses; True IDE PIO, no interrupts; GLUE hard-codes wait states (7@12MHz, 12@20MHz)
* **IO:** the AT89S52 IO MCU (PS/2, UART, systick) was abandoned — its weak P2 pull-ups could not drive the data bus against whatever held it during IO MCU cycles (investigation in griffin.log).  The XR68C681 DUART replaced it on a DIP carrier; PS/2 moved to a GLUE frame engine.  Working: serial ch A 115200 8N1 (interrupt-driven RX + polled TX, crt0 brings it up early), SYSTICK at 100Hz from the C/T (level 5), PS/2 RX and TX through GLUE (level 4).  Ch B on a TTL header.
* **Audio (rev 1) — CPU-driven 8-bit:** a '373 latch clocked by GLUE's ~AUDIO\_LE on CPU writes to the audio address, into an 8-bit R2R + LM358; no FIFO, no DMA.  Two supported patterns:
  * ISR-driven (OS-friendly, ~8-11 kHz): the 68681 C/T fires periodically; the ISR writes one sample.  Ceiling set by ISR overhead with ROM wait states — probably 8-11 kHz before the ISR eats most of the CPU.  Good enough for CP/M-68K or Fuzix.
  * Busywait-driven (game-friendly, up to ~31/15/10 kHz): VIDEO STATUS bit 0 toggles once per visible line (31.469 kHz at VGA 640x480@60); code polls the toggle, then writes AUDIO; /2 or /3 by skipping lines.
  * The VIDEO→U23 AUDIO\_LE bodge (VIDEO pin 36) went unused.
* **Buzzer** — replaced by the speaker in Rev 2.

## Bodges on the rev 1 board

Each entry is either folded into the Rev 2 schematic natively or carried as a design lesson.

DEBUG\_OUT LED:

* small NPN like a 2N3904 or SOT-23 MMBT3904. Collector to \+5V through the LED and resistor, base to the DEBUG\_OUT pad through a 1K–10K resistor, emitter to ground. The base current is microamps so it won't load the serial line at all, and the LED gets a clean 5V drive independent of your logic levels.  
* You could dead-bug it right across the two pads - body of the transistor sitting on top, legs bent to reach the resistor and LED. A little ugly but perfectly functional for a dev board.
* See also bodges in [griffin.yml](griffin.yml)

DUART on the IO MCU carrier:

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

# Rev 3

*(Rev 3 is Rev 2 plus the deltas below.  Anything not listed here is as Rev 2.  Parts of this section were drafted before the Rev 2 video, serial and PORTS decisions; those entries have been trimmed to the delta.)*

## SW Investigation Plan

???

## Possible new hardware features (July 24 2026)

Video through a CLUT RAM?  For 640 pixels wide need a ~15-25ns part, don't have one in my kit

## Strategy

* Use Rev 2 strategy plus any revelations gathered along the way

## Rev 3 components

*(Parts and their function — the Rev 3 deltas.  Footprints, nets, placement, and layout gotchas live in Board changes, not here.)*

* **CPU, clocking, CF, RTC, PORTS, joysticks, audio, interrupts:** as Rev 2.
* **RAM:** 16-bit-wide async SRAM, 8 MB.  One AS6C6416 (4M×16 = 8 MB, 55 ns) at 0x000000–0x7FFFFF; GLUE decodes a single chip select (A23==0), byte lanes from UDS/LDS — no SDRAM/PSRAM, no memory controller.  (Was 2 chips / 12 MB; 0x800000–0xBFFFFF is now the flash window — see ROM.)
* **ROM:** 1–2× M29F160FB (16 Mbit 5 V NOR, 1M×16, 55 ns, TSOP48), soldered — grows the Rev 2 SST39SF040 pair to make room for a Linux kernel + erofs root in flash, which 1 MB cannot hold.  512K×8 is the ceiling for 5 V flash in DIP (A18 is the top address pin of the 32-pin JEDEC pinout), so going past 1 MB is exactly what forces the move to TSOP and hence to an SMT respin.  Still in production (Alliance Memory second source; DigiKey/Mouser/TME stock).  FB (bottom boot) preferred: the 16/8/8 KB boot sectors sit at the vector/bootloader end while the erofs image lives in the uniform 64 KB sectors above (FT differs only in sector order).  ×16 mode: BYTE# tied high, flash A0–A19 ← CPU A1–A20, D0–D15 direct — one chip puts 2 MB on the full 16-bit bus (vs 1 MB from the SST pair); an optional second chip selected by A21 makes 4 MB.  Flash window stays at 0x800000–0xBFFFFF as in Rev 2.  55 ns may allow zero-wait ROM at 14 MHz.  Initial flash and recovery via the on-board RP2350B bridge's UF2 drive (see below), which bus-masters JEDEC cycles into the flash window with the CPU held off — this is what buys back the recovery path Rev 2 gets for free from its sockets.  (Offline fallback: XGecu T48 + ADP_F48_EX-1 TSOP48 adapter)  The CPU self-reflashes in-circuit via the same GLUE ~ROM_WE / CONFIG.FLASH_WE_EN mechanism carried forward from Rev 2, except that a single x16 part takes JEDEC command sequences at word addresses 0x555/0x2AA directly rather than Rev 2's byte-duplicated-into-both-lanes form; flasher still runs from RAM (not read-while-write).  RESET# to the ~RESET net.
* **GLUE ATF1508:** as Rev 2, plus a ~BOOTSTRAP input from the RP2350B bridge or test header: while asserted, ~ROM_WE follows flash-window writes regardless of CONFIG.FLASH_WE_EN, and any AS-stuck watchdog is relaxed so slow external bus cycles aren't killed.
* **Serial I/O:** as Rev 2 (DB-25 RS-232 both channels).  The FT2232H bridge below rides console channel A in parallel with the RS-232 stage.
* **RP2350B bus-master (USB-C #1, on-board):**   47 of 48 GPIOs: A1–A23 (23 — A23:22 must be driven to reach the flash window at 0x800000 and the rest of the map), D0–D15 (16), ~AS/~UDS/~LDS/R/~W (4), ~DTACK (1), ~RESET/~HALT/~BOOTSTRAP (3, open-drain on the existing nets).  USB rides the dedicated DP/DM pins (zero GPIO), and no VBUS-sense pin is needed: its USB-C is the board's power input, so VBUS is present whenever the board is on — dumb charger and host-peripheral both work — which also guarantees the powered-5V-tolerance precondition; no level translators.  USB device = **UF2 virtual-FAT drive** (GRIFFIN volume with INFO_UF2.TXT and a CURRENT.BIN readback file; host writes rom.uf2 → assert ~RESET+~HALT+~BOOTSTRAP, bus-master JEDEC erase/program/verify into the flash window, release reset on eject) plus a CDC command channel for scripted flashing and peek/poke — the bring-up tool: read/write RAM and peripheral registers with no working CPU or ROM.  Self-programming via the RP2350 ROM's BOOTSEL USB bootloader on its own port; SWD test points as backup.  Safety: GPIOs default Hi-Z at reset; watchdog so a crash can't stay driving the bus; external resistors, not internal pull-downs (erratum RP2350-E9).  If the one spare pin ever isn't enough, the ~BOOTSTRAP-gated 74HC595 pair on the upper address lines frees ~13.
* **FT2232H bridge (USB-C #2, data-only):** Channel A (MPSSE) = JTAG to the CPLD chain; B = console ↔ DUART Ch A.  Self-powered from the board 5 V rail (shared 3.3 V LDO); its connector does **not** feed the rail — VBUS goes only to an enumerate-when-cabled sense divider, so there is no back-power path with two hosts attached.  3.3 V and not 5 V-tolerant: lines driving *into* it (CPLD TDO, DUART TxD) get level translation; 3.3 V→5 V lines drive direct.  12 MHz crystal + optional 93LC56 EEPROM.
* **Debug headers:** as Rev 2, plus ~BOOTSTRAP on header 1 (exactly 24), and the backup bus-master role now covers the case where the on-board RP2350B is the thing being debugged or its firmware is bricked mid-development.  Programming through the headers is flash-limited (~10 µs/word ⇒ ~20–30 s per 2 MB chip) and adds JEDEC unlock/erase/program/verify to the bus-cycle mechanics.  The 1×6 JTAG strip stays live even if the FT2232H path is broken; series resistors between the FT2232H and the JTAG lines let the strip cleanly overdrive an idle FTDI.
* **Video:** as Rev 2 (super-engine).  Open: the CLUT RAM idea above; a bitfile-only 2bpp mode if it fits.
* **Power:** USB-C #1 (RP2350B) is the board power input; see Board changes.  This reverses the Rev 2 decision and needs the Rev 2 "why not USB-C" objection answered (PD trigger or Rp sensing).

## Board changes

*(KiCad watch-list — footprints, pinouts, nets, placement, SI.  Function lives in Rev 3 components above; link, don't restate.)*

### Footprints & sockets
- [ ] ROM: 1–2× M29F160FB TSOP48, **soldered** (image-zero via the RP2350B bridge; bench T48 + ADP_F48_EX-1 as fallback); route ~ROM_WE (carried over from Rev 2), /OE←nR_W, /CE←nROM_SELECT (2nd chip select by A21), BYTE#→+5V, RESET#→~RESET net
- [ ] RAM: 1× AS6C6416 TSOP-II; /UB←nUDS, /LB←nLDS, /OE←nR_W
- [ ] RP2350B: QFN-80 + QSPI flash + 12 MHz crystal; own USB-C (#1 = board power input, rear); BOOTSEL button + SWD test points; route A1–A23, D0–D15, ~AS/~UDS/~LDS/R/~W, ~DTACK, ~RESET/~HALT/~BOOTSTRAP (47 of 48 GPIOs — one spare)
- [ ] FT2232H: USB-C #2 (data-only, rear); 12 MHz crystal, optional 93LC56 EEPROM; port A JTAG → CPLD chain through series isolation resistors (JTAG backup lives on the debug header, no separate 2×5), port B ↔ DUART Ch A
- [ ] decide SMT vs THT for CF, RAM, DUART, and the CPLDs
- [ ] Debug headers: as Rev 2 plus ~BOOTSTRAP on header 1; keep stubs short; route ~BOOTSTRAP to GLUE (and the RP2350B bridge)

### Power & decoupling
- [ ] USB-C #1 (RP2350B) with two CC sink resistors is the board power input (no PD — Rev 1 draws 0.84 A on a plain 5 V sink; power-only charger and host-peripheral both fine, hosts advertise ≥1.5 A on C); USB-C #2 (FT2232H) is data-only — VBUS to a sense divider, never the rail, so no back-power path
- [ ] decoupling cap on every +5V/GND pair, especially the CPLDs
- [ ] 3.3 V LDO for the RP2350B (IOVDD; core is internally regulated)

### Pull-ups
- [ ] as Rev 2, plus ~BOOTSTRAP

### Signal integrity & analog
- [ ] as Rev 2
- [ ] 7200s on the bus may need transceivers for capacitance if the SMT respin changes the loading — re-run the Rev 2 electrical review
- [ ] level translation on the 5V→3.3V lines into the FT2232H (CPLD TDO, DUART TxD); the RP2350B needs none (5 V-tolerant while powered, and always powered with the board)

### Test & debug access
- [ ] as Rev 2
- [ ] wire all CPLDs into the JTAG chain — also the boundary-scan-flash path

### Carryover bug fixes
- [ ] whatever is still open in the Rev 2 list when Rev 2 is done

### Process & layout
- [ ] HDL first, as Rev 2: bring every CPLD bitfile to a fit with the fitter free to minimize macrocells, then freeze the pinout before routing
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
