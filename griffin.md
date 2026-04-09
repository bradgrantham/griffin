# What is it?

Computer related to parts I already have in the bin

Start with baremetal fun, then progress towards full SpareMiNT or fuzix until bored
*  [Atari ST Free Operating Systems - Vincent Rivière](https://youtu.be/28ieOWEQXhU?si=ekVV36ixjHvCfm06&t=1301)  

MVP:

* Fuzix or CP/M-68K with an image viewer: NTSC 704\*480i BW, NTSC 176\*480i color (704 sample artifact color) , VGA 640\*480p 2 colors per row from palette of 256, VGA 320\*480p 4 colors per row from palette of 256

# References

68000 cycle counts - [https://gist.github.com/cbmeeks/e759c7061d61ec4ac354a7df44a4a8f1](https://gist.github.com/cbmeeks/e759c7061d61ec4ac354a7df44a4a8f1)	

Use PLD or CPLD devices - **settled on ATF1508 PLCC-84**

* GAL22V10 - [ATF22V10C-10PU Microchip Technology | Integrated Circuits (ICs) | DigiKey](https://www.digikey.com/en/products/detail/microchip-technology/ATF22V10C-10PU/1775078)  
  * [https://github.com/daveho/GALasm](https://github.com/daveho/GALasm)   
* CPLDs  
  * ATF750 - [ATF750CL-15PU Microchip Technology | Integrated Circuits (ICs) | DigiKey](https://www.digikey.com/en/products/detail/microchip-technology/ATF750CL-15PU/1914513)   
  * [ATF2500C-20PU Microchip Technology | Integrated Circuits (ICs) | DigiKey](https://www.digikey.com/en/products/detail/microchip-technology/ATF2500C-20PU/1008426)   
  * PLCC-44 [ATF1504A S-10JU44 Microchip Technology | Integrated Circuits (ICs) | DigiKey](https://www.digikey.com/en/products/detail/microchip-technology/ATF1504AS-10JU44/1118923)   
    * [PLCC-44-AT Adam Technologies PLCC Socket | Jameco](https://www.jameco.com/z/PLCC-44-AT-Adam-Technologies-44-Pin-PLCC-Socket-Soldertail-Through-Hole_71618.html)   
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
  * Use pyftdi for toggling gpios and reading the results for test vectors  
  * **~~Sourcing~~** ~~ATF1508s may be difficult.  May need to stockpile?~~ Plenty at Microchip for now.
  * [https://www.youtube.com/watch?v=LnGaDpGbbjQ](https://www.youtube.com/watch?v=LnGaDpGbbjQ) has a bunch of details on HDL through Microchip's tools and using these devices  
  * Really need to write HDL **before** doing the PCB because some pins may need to move.

# Design philosophy

Try to do something more long-term sustainable that you can pick up and restart more easily  
Use Verilog and verilator  
Find a way to share constants between Verilog, linker.ld, crt0.s, and C++

* Codegen from a master file

How much design file can be in YAML or in Python?  Generate from YAML:

* Constants in Verilog, linker.ld, crt0.s, and C++  
* Check against netlist from KiCAD

# Case and form factor

~~Maybe target a standard case form factor (e.g. Micro-ATX or Mini-ITX) - use your existing old case?~~

* ~~Could use existing cases and power supplies~~  
* ~~Need a “PS\_ON” circuit~~  
* ~~3D print I/O panel snap-in~~  
* ~~\*could\* have pin-header to screw in slot plate for HCI~~  
* ~~Most cases have power LED, button on front, USB ports, headphone jack~~  
* ~~E.g. [https://www.amazon.com/Goodisory-MX01-Fanless-Chassis-Vertical/dp/B07T2HKWZN](https://www.amazon.com/Goodisory-MX01-Fanless-Chassis-Vertical/dp/B07T2HKWZN)~~  
* **Nah.**  
* **Use USB\_C and print a case.**

# Continuing Board Design To-Do

* Silk screen for items esp test points  
* Need BOM output but some way to select “I have these already”.  
* Footprint issues  
  * PS2 stab lock pins and need wider strain relief drill  
  * Need holes for retainer feet for headphone jack  
  * Rca jack needs to be past end of board for retainer feet?  Or square holes for pressure fit?  
* No connection from VIDEO to supply DTACK to GLUE, and no signal back to VIDEO to IACK.  Are these important?
* If the CPU clock was gated through GLUE, other peripherals (mostly thinking ENGINE) could perform fast bus operations while stalling CPU clock. -> GLUE has access to HALT, so it can stop the CPU if that works out.
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
  * For PCB Rev1, IO MCU is bypassed; UART is via GLUE bit-bang on DEBUG_OUT/DEBUG_IN.  Current status:
    * *Working* UART TX at 115200 via GLUE_TIMER + DEBUG_OUT (`timer_putchar`)
    * *Working* UART RX at 115200 via GLUE_TIMER + DEBUG_IN polling (`debug_getchar_asm`)
    * *Working* SYSTICK at ~183Hz from GLUE (SYSCLK/65536) - IRQ gated by CONFIG register
    * *Working* CF card with FatFs - can mount and list FAT filesystem
    * *Need to get to reliable streaming serial so I can send and receive data to some kind of network device e.g. esp32*
    * GLUE can generate level-4 IRQ on DEBUG_IN falling edge (start bit) for interrupt-driven RX
    * PS/2 clock latches the data line and causes interrupt, PS/2 shares an interrupt and exposes which clk through status register


## Need to buy

* crystal for 68681

# Bodges for rev 1 Board

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

# Debug the GLUE

Bodge pullups on JTAG TMS 

Try to set fitter parameters to disable GCLR, OE1, OE2

Maybe set Verilog to 

# Components

## Power

USB-C power with switch inline between supply and USB-C port

* “two 5.1k pull-downs on both CC pins of your connector” - check for 1A delivery, test this

## Power-on reset and user reset button

RC reset/halt trigger - short RC to discharge with reset button

## System clock

CPUCLK - 12MHz clock or 20 MHz from TTL oscillator, okay to change and reflash GLUE bitfile if clock is changed

## CPU

68000P12, upgradeable to 68EC000-20 or 68010@10 or any 68K in 64-DIP format

* Redo board to slot in a 68030 at some later date if desired

## RTC

completely forgot from the beginning.

* Full up on pins in GLUE and in IO MCU, so not easy to add now and board space may not be available  
* Add a revision later, use the BQ3285 and a coin cell to be somewhat period-appropriate

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
* GLUE_TIMER: 5-bit auto-reload timer with ÷8 prescaler (shared with systick).  Writing a non-zero value to TIMER loads the period and starts a free-running countdown; period = (N+1) × 8 SYSCLK cycles (N=1..31, range 16..256 clocks).  Writing 0 stops it.  Writing TIMER_ARM sets an armed flag that blocks ALL bus DTACK (freezing the CPU) until the timer countdown reaches zero, then auto-clears.  This provides deterministic bit timing for UART without cycle-counting.
* UART TX at 115200 baud via GLUE_TIMER + DEBUG_OUT: firmware sets TIMER period to 12 ((12+1)×8=104 clocks ≈ 115384 baud at 12 MHz), then for each bit arms the timer and writes DEBUG_OUT.  The arm stall absorbs variable instruction timing so each bit is exactly 104 clocks.  Implemented in crt0.s `timer_putchar`. A fallback 9600 baud bitbang TX (`early_putchar`) exists for pre-timer debugging.
* UART RX at 115200 baud via GLUE_TIMER + DEBUG_IN: firmware polls DEBUG_IN for start bit, then uses TIMER arm to sample each data bit at the correct interval.  Implemented in crt0.s `debug_getchar_asm`.  
* Systick: always-running ~183 Hz periodic interrupt (SYSCLK ÷ 65536, sharing the ÷8 prescaler then dividing by 8192).  IRQ gated by CONFIG.SYSTICK_IRQ_ENABLE; timer always runs and pending flag always sets regardless.  Reading SYSTICK_STATUS clears the pending flag and deasserts the IRQ.
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
  * Primary UART TX output — clip FTDI RX to test point, 115200 8N1 via GLUE_TIMER  
* DEBUG_IN
  * Reads test point input  
  * Primary UART RX input — clip FTDI TX to test point, 115200 8N1 via GLUE_TIMER  
  * Can generate level-4 autovector IRQ on falling edge (start bit detection)
* Registers: see [griffin.yml](griffin.yml).

## VIDEO

[Griffin Video Mode Throughput](https://docs.google.com/spreadsheets/d/1jpam0LNxlgqLVfV4WW1wBMNDqXu4QpefYhichfac1WE/edit?usp=sharing)

NTSC, VGA pixel and timing generation - second ATF1508

* CPLD 16-bit shift register clocks out 1 bit, expands to R3G3B2 through internal pair of palette registers  
* Count off hsync and vsync to provide HSYNC and VSYNC signals and exit-VBLANK interrupt (through GLUE)  
* Registers: see [griffin.yml](griffin.yml). All config registers default to 0 (video disabled). Some can be changed at any time but in practice CPU is in a tight pixel loop during visible lines, so changes happen in hblank or vblank.
* MODE: To change modes, disable ENBVINT, wait at least 50ms for settling, configure other registers, then write MODE.
* MODE2 PPC: Realistically upgrading to a 68EC000@20 will be needed to get 640×480.
* ARM\_SNOOP: Write to arm blocking snoop (VIDEO\_STALL to GLUE after CPU releases \~AS). Must be written once per visible line.  
  * Snooped bus user data (FC2:FC0 == 101\) in “slow palette” mode expects 16bits of pixel data (16 pixels) repeated through visible pixels  
  * Snooped bus data in “fast palette” mode expects 16 bits of palette (2x R3G3B2) and then 16bits of pixel data (16 pixels) repeated through visible pixels and then one more 16-bits palette  
    * Would need something like looped or unrolled “move.l (A0)+, D0”, transfer two 16-bit words in 12 cycles. - works for 68000 @ 12MHz\!  
    * Palette data loaded into “next palette”  
    * Next palette loaded into palette when shift register is loaded  
    * If there is no waiting pixel data, shift register and palette are loaded from border pixel and next palette.  
  * CPLD holds VIDEO\_STALL until shift register is empty  
    * Latches 16 bit shift register from data lines on PIXEL\_CLOCK signal  
    * Releases VIDEO\_STALL on next CPU\_CLOCK cycle so GLUE ~DTACK is asserted, then asserts VIDEO\_STALL when CPU releases ~AS  
  * If VIDEO\_STALL is disabled, CPU is responsible for managing timing, or maybe framebuffer is used as visual debugger  
  * Must be written once per visible line  
  * Notes  
    * For VGA only, "slow palette" is the palette is updated at most once per line, or perhaps as slow as set once and never set again.  "Fast palette" is the palette is updated every 16 pixels. For composite, the pixels are just 0 and 1 and I might add a colorburst mode so I can get artifact colors.  
    * 16-bit "pixel shift register", "palette register", "next 16 pixels" register, and "next palette" register. The LSBit of the pixel shift register is the current pixel, and selects from the two 8-bit values in the palette register for VGA.  Or it is black or white voltage for composite video.  Every pixel clock the pixel shift register shifts right.  On the starting pixel clock (multiple of 16\) and every 16 clocks through the end of visible pixels, the pixel shift register would be loaded from the "next 16 pixels" and the palette register would be loaded from "next palette".  After the visible pixels range, every 16 clocks until visible pixels starts again, the shift would be filled with border pixel (maybe set or cleared) and the palette register would be loaded from VIDEO\_BORDER\_PALETTE. The implication is that all lines will be multiples of 16 and I'm okay with that.  
    * in "slow palette" mode, stall the next bus access (expecting a CPU read from framebuffer) until the pixel shift register is loaded from the previous "next 16 pixels", at which point the "next 16 pixels" are loaded from the snoop, the bus D lines are loaded into "next 16 pixels" and STALL is released.  Thus a line of "slow palette" can optionally set the "next" palette register to set border colors for the line, write `VIDEO_ARM_SNOOP`, then just read N words. Next palette register isn't set during the line so it can still get loaded into "palette register" every 16 clocks.  
    * In "fast palette" the snoops also set the "next palette register" first, so a line of "fast palette" writes `VIDEO_ARM_SNOOP`, then reads N\*2 words made of a palette read and a pixel read, likely a move.l which will then perform two word reads. The first load will not stall but will be immediately loaded into "next palette". The second read will stall until the shift is loaded from the "next 16 pixels", the bus D lines are loaded into "next 16 pixels" and STALL is released.  
* DISARM\_SNOOP: Write to disarm blocking snoop.
* Just before beginning of frame VIDEO interrupts CPU so that CPU enters “framebuffer” ISR, doing N (e.g. N is 200, 240, 400, 480\) tight loops to write rows and then check IO MCU, etc in HBLANK, exits ISR at end of visible rows   
  * Tight loop writes out 16 bits at a time for 320 or 640 pixels, eg MOVE.W (An+), D0  
  * CPU does busy wait on vblank todo per frame processing  
  * CPU ISR sets some word in memory at vblank  
  * CPU out-of-ISR loop waits on that word, then clears it, then does vblank processing  
* In HBLANK / HSYNC:  
  * Maybe check UART/PS2, dequeue byte - but what if IO MCU is in an ISR of its own, that could take 10s to 100s of 68000 cycles to finish…  Probably just have long queues in the MC and let vblank processing drain queues.

## Compact Flash interface

* Only do 8-bit access to ease routing, D0-D7 so only odd addresses  
* Entirely True IDE PIO mode, no interrupts  
* GLUE manages DTACK, will need to hard-code wait states as necessary (7@12MHz, 12@20MHz)  
* Registers: see [griffin.yml](griffin.yml).

## IO processor

Keyboard, mouse, serial port through 8051-compatible AT89S52

**Unlikely to work on Rev 1 PCB.**  The AT89S52's P2 port uses weak internal pull-ups (~50 uA) to drive D0-D7, and something on the board is driving the data bus during IO MCU cycles that the pull-ups cannot overcome.  Extensive debugging has ruled out every other chip on the bus (see "Continuing Board Design To-Do" for full debug log).  Communication only worked when the board was running abnormally slowly due to logic analyzer interference with CLK.  For Rev 1, UART is handled by GLUE bit-bang via DEBUG_OUT/DEBUG_IN at 115200 baud.  Rev 2 should add a 74HC245 buffer between AT89S52 P2 and D[7:0], or replace the AT89S52 with a part that has proper bus drivers (e.g. 68681 DUART).

* [AT89S52-24PU Microchip Technology | Integrated Circuits (ICs) | DigiKey](https://www.digikey.com/en/products/detail/microchip-technology/AT89S52-24PU/1008597)  
* 5V UART, just TX, RX  
* 2 PS2  
* AT89S52 Continuously polls IO\_SELECT\_MOSI from GLUE chip: if detected, disable interrupts, do 68000 bus cycle including putting data on data bus, lowering DTACK, then waiting for AS to rise and releasing DTACK, enable interrupts  
* Need FIFO for all inputs so CPU doesn't need to do anything during visible row scanout ISR  
* Registers: see [griffin.yml](griffin.yml).
* Program either in jig or by GLUE control signals  
* ISR for UART, PS2  
* Got that old PS/2 software from PIC for Alice 2  
* I screwed up; kbd and mouse clocks needed to go to P3.2 and P3.3, and I moved them to non-interrupt-capable pins without thinking about it.

## ENGINE - DMA

Third ATF1508AS.  HALT-based bus-stealing DMA controller for video (and audio).

* Reads framebuffer data from SRAM and signals VIDEO to latch D[15:0] directly from the data bus
* Uses HALT-based bus stealing: ENGINE asks GLUE to halt the CPU, then drives the address bus to perform an SRAM read while VIDEO snoops D[15:0] via LATCH signal
* ENGINE does not know or care about pixel format — it is a word pump.  VIDEO controls how many words per line via NEED\_WORD, and signals EOL to advance ENGINE to the next row
* Audio: not handled by ENGINE in Rev 1.  The original plan (VIDEO requests an extra ENGINE transfer per line and asserts AUDIO\_LE instead of LATCH, so the audio DAC captures D[15:0], with the sample stashed at the end of each row) was abandoned because every usable rate tightly couples to HSYNC and the framebuffer layout gets awkward.  See "CPU-driven 8-bit audio" below.
* Row stride is always a multiple of 64 words (128 bytes).  CPU configures stride via a 2-bit field: 0=64, 1=128, 2=192, 3=256 words.  Progressive uses stride = smallest multiple of 64 >= active words.  Interlaced uses 2x that to skip the other field's line in a line-sequential framebuffer.
* ADVANCE register (write-only command) performs the same row-advance operation as EOL; used by VSYNC ISR to offset field 1 by one line

Bodge wires required on Rev 1 (6 total):

* ENGINE pin 2 (OE2) <-- VIDEO: NEED\_WORD (VIDEO shift reg needs data)
* ENGINE pin 8 <-- VIDEO: SOF (start of frame, reset pointer)
* ENGINE pin 10 <-- VIDEO: EOL (end of line, advance to next row)
* ENGINE pin 40 --> VIDEO: LATCH (D[15:0] stable, capture now)
* ENGINE pin 6 (was \~ENGINE\_IACK) --> GLUE: HALT\_REQ (request CPU halt)
* ENGINE pin 9 <-- GLUE: BUS\_FREE (CPU halted, bus available)

Fits 108/128 macrocells (84%) with current register set.

## CPU-driven 8-bit audio

The '373 audio latch is clocked by GLUE's ~AUDIO\_LE on CPU writes to the audio address; there is no hardware FIFO or DMA engine.  Driving the DAC is a CPU timing problem, with two supported patterns:

* **ISR-driven (OS-friendly, ~8-11 kHz).**  GLUE timer (or a future VIDEO line IRQ) fires periodically; ISR writes one sample and returns.  Ceiling is set by ISR overhead on the 12 MHz 68000 with ROM wait states — probably 8-11 kHz before the ISR eats most of the CPU.  Good enough for a general-purpose OS that must also do other work (CP/M-68K, Fuzix).
* **Busywait-driven (game-friendly, up to ~31/15/10 kHz).**  VIDEO exposes a STATUS register whose bit 0 toggles once per visible line (v\_cnt[0]; 31.469 kHz at VGA 640x480@60).  Code polls the toggle, then writes AUDIO.  1x coupling = 31.469 kHz (one sample per flip).  /2 or /3 rate by skipping 1 or 2 lines.  A game that gives up its main loop to audio-plus-framebuffer-writes can spend every non-rendering cycle on audio.

This leaves the VIDEO→U23 AUDIO\_LE bodge (VIDEO pin 36) unused in Rev 1; future revisions may repurpose the pin.

* 8-bit R2R  
* [LM358](https://www.digikey.com/en/products/detail/texas-instruments/LM358P/277042) op-amp  
* Timing jitter from variable instruction execution and ENGINE-induced HALT will modulate audio; expect audible noise during heavy ENGINE activity.  Mitigations: (a) keep the audio write at the same position inside the busywait loop so HALT-induced jitter is constant per-sample, (b) one-pole RC filter on DAC output, or (c) disable rendering during audio-critical moments.

# Rev 2

- [ ] Schematic (+PCB if necessary)
  - [ ] Split VIDEO into PIXEL and TIMING CPLDs to give more flexibility
    - [ ] TIMING could handle more complex timing modes (like what?  There's a 25MHz oscillator) or possibly DMA modes
    - [ ] PIXEL could handle more pixel formats, deeper FIFO, more palette modes
  - [ ] Compile bitfiles for CPLDs and let fitter assign pins in order to let macrocell count be minimized
  - [ ] Determine a more available ROM technology and design around that.
    - [ ] CPLD with I2C ROM?
    - [ ] RP2350?
    - [ ] Move to 16-bit ROM and commit to OneROM - can it go at 70ns?  I guess I can always wait-state to match if it's slower.
    - [ ] https://www.digikey.com/en/products/detail/microchip-technology/AT27C4096-90PU/1008614 is a 256K x 16 ROM, 40DIP, 90ns (more wait states but maybe okay) for about $10 and they have 142 of them at the moment
  - [ ] Replace AT89S52 with 68681 DUART (same DIP-40 socket) — see below
  - [ ] Pullups on JTAG lines
  - [ ] Pullup on DTACK so missing peripherals can't spuriously ACK
  - [ ] 4.7K Pullup on HALT
  - [ ] Pullups on anything between GLUE and VIDEO and ENGINE and IO in the case of any of VIDEO/ENGINE/IO not being populated
  - [ ] Kill ENGINE BGACK, BG, BR, DTACK, IACK, FC lines; it stops CPU through GLUE through HALT and reads memory assuming a particular access speed, CPU autohvectors
  - [ ] Rework GLUE IO MCU signals for 68681: ~IO_SELECT becomes ~CS, ~IO_DTACK becomes a pass-through input (68681 drives DTACK), ~IO_IRQ stays as interrupt input; drop ~IO_IACK (tie 68681 ~IACK high, use autovectors, read ISR to clear)
  - [ ] Route oscillators separately into VIDEO for simplicity, if possible  
  - [ ] More signals between GLUE, VIDEO, ENGINE
    - [ ] Could I squeeze 16 bits for a bus from ENGINE to VIDEO?  Or even just 8?
  - [ ] Decoupling caps for every +5V/GND pair especially CPLDs
  - [ ] GND, +5V, D0-D15, A1-A10, WRITE_LO, WRITE_HI, IO/VIDEO/ENGINE/AUDIO select/latch, nVPA, nBERR to test points
    - [ ] use a pin header expecting Dupont jumpers to logic analyzer or use a jumper to a scope probe
    - [ ] Make the pin header be 2xN, down each side silk screen the signal at the pin
  - [ ] Pullups on PS/2 clock lines
  - [ ] Make SYSCLK go into a GCLK on CPLDs especially GLUE
  - [ ] Make audio stereo - one 16-bit write
    - [ ] If this was wired to ENGINE instead of to the bus then ENGINE could pick up the next sample(s) any time and latch them at the right time (at end of a scanline)
  - [ ] Wire ENGINE CPLD into the JTAG chain, free up GLUE signals to ENGINE JTAG
  - [x] Put in a driver for debug LED so it doesn’t interfere with debug out voltage level  
  - [ ] Put USB-C with PD on the board
  - [ ] Much more attention to analog components - redesign composite and VGA analog circuitry to be robust
  - [ ] 68681 DUART replacing AT89S52 (same DIP-40 socket, proper bus drivers, two UARTs, counter/timer, parallel I/O)
    - [ ] Wire D0-D7, R/~W directly to 68681
    - [ ] Wire A1-A4 to RS1-RS4 (16 registers, 4 select lines)
    - [ ] Wire ~RESET directly from ~RESET net (active-low; AT89S52 RST was active-high via IO_RESET from GLUE — no longer needed)
    - [ ] 3.6864 MHz crystal on X1/X2 for clean baud rate division (replaces MCU Y2 crystal)
    - [ ] Tie ~IACK high (autovectors); frees one GLUE pin
    - [ ] Channel A: terminal UART (replaces DEBUG_OUT/DEBUG_IN bit-bang); Channel B: ESP32 or other network device
    - [ ] IP0-IP3 have input-change-detect interrupt — could use for PS/2 clock edge detection
    - [ ] OP0-OP7 directly drive RTS/CTS flow control and other active-low output signals
    - [ ] PS/2 keyboard/mouse: 68681 IP/OP pins are not bidirectional open-drain, so PS/2 may still need external open-drain buffers or a separate solution
  - [ ] Maybe
    - [ ] Put two FTDI's on the board with USB-C for debug output and for 68681 serial console
    - [ ] Audio input
    - [ ] RTC - manage through MCU?  Maybe multiplex through A/D?
    - [ ] Slap an ESP on it or a Pico for networking?  Moving to a Pico for UART, keyboard, mouse, timer, and wifi would knock a lot of stuff over in one shot, but unclear whether 3.3V would be a problem.
  - [ ] CF symbol is junk - redo it.
  - [ ] CF card IOWR should be gated by AS.
    - [ ] CF card latches on rise of IOWR
    - [ ] If just the 68000's R/~W passed through, then AS is long gone and data may be junk at time of rise of IOWR.  Fix is to combine them through GLUE.
  - [ ] CF card to 16 bits?  Routing those pins will be annoying, but possible.
- [ ] PCB only
  - [ ] CF card DMACK to +5CF card CS0 and CS1 are swapped!!  Fix them for now in Verilog, revisit Verilog and PCB for rev 2
  - [ ] Do more of a hub-and-spoke kind of model, run bus and signals across from CPU, put peripherals above and below with vertical taps
  - [ ] PS2 stabs - move footprint  
  - [ ] Headphone jack pads - drill partial holes?  
  - [ ] RCA jack retainer feet - drill partial holes?
  - [x] Swap MOUSE\_CLK and KBD\_DATA  
  - [x] Route A18 to GLUE instead of A6  
  - [x] Wire GLUE VPA back to the CPU in place of ENGINE\_IACK
  - [ ] Decoupling for ROM is too close to the socket if I will be using ZIF - need ZIF footprint
  - [ ] Crystal and decoupling for MCU is too close to the socket if using ZIF - need ZIF footprint
  - [ ] Should design the pin header (like, what part number) into the JTAG, the Adafruit USB-C BOB, and the FTDI serial connector footprint
  - [ ] Remember that the FT232H breakout should probably be USB-C cable to the rear of the board, so rotate it 90 degrees counter-clockwise and try to leave real estate for it
    - [ ] Is there a castellenated version I could solder on?
    - [ ] Is there a better version, something smaller with fewer pins?
  - [ ] Flip FTDI - it's 180 degrees so I have to currently put FTDI upside down onto 90-degree header
  - [ ] Put in lots of holes for ground test points around the board



---

# Older Notes - details

## On board video

* 912 samples \* 262 @ 59.9 is 14.318MHz.  
  * 1 bpp = 14.318 Mb/sec =  .89 MT/sec  
* 400 \* 525 (320 \* 480\) is 12.6MHz, repeat lines and double pixels to 640 \* 480  
  * 1 bpp  = 12.6 Mb/sec = 787 KT/sec  
  * 4 bpp = 50.4 Mb/sec  = 3.15 MT/sec, tight and need \>= 16MHz 68010 probably  
  * 8 bpp = = 6.3 MT/sec - not possible  
* 800 \* 525 @ 60 (640 \* 480\) is 25.2MHz,  
  * 1 bpp  = 25.2 Mb/sec = 1.574 MT/sec  
  * 4 bpp = 100.8 Mb/sec  = 6.3 MT/sec, not possible  
  * 8 bpp = = 12.6 MT/sec - not possible  
* ~~16-entry 8-bit (R3G3B2) palette - how to write?~~  
  * ~~4 address bits, 8 data bits through CPLD gated on palette address space~~  
  * ~~16-entry VGA palette RAM is at 0x20-0x2f~~  
    * ~~Address lines into palette RAM are out of CPLD, normally part of shift register, but also enabled by writes to 0x20-0x2f~~  
    * ~~Palette RAM 8-bit data lines will be wired to the data lines out of the CPLD and to the DAC, so must only update the palette RAM during sync when the VGA monitor is not looking at RGB~~  
    * ~~The smallest cheapest sram is 8k…  Might have enough CPLD pins for a “palette selector” index on higher palette SRAM address pins.~~

Out of scope

* MMU - bail for now  
  * 5 bits of PID, 4K page size  
  * 2x 128KByte SRAMs replace the top 12 address bits.  
  * Pair of ‘245s between CPU and bus and another pair between SRAMs and bus  
  * CPLD controls whether CPU or SRAM sends address, CPU on boot  
  * CPLD also writes config to SRAMs, adds wait states  
* Networking  
  * Claude suggested these chipsets:  
    * RTL8019/8029 (NE2000-compatible ISA chips)  
    * SMC91C92/91C111 (LAN91C111 family)—this is promising  
    * CS8900A (Cirrus Logic, used in some Atari ethernet adapters like the EtherNEC and NetUSBee)

HCI board

* ~~NTSC monochrome, artifact color~~  
  * ~~Drive a 19-bit counter for pixels - 3 are for shift register or 8-to-1 for pixels, 16 are for ROM and RAM byte address~~  
  * ~~128K timing / signal ROM can do monochrome 912x525 progressive in lower 64K, interlaced in upper 64K~~  
  * ~~704 pixels means 88 bytes are active but 912 samples means 114 bytes per row~~  
  * ~~Could clock out a byte to latch and audio R2R DAC at byte 88~~  
  * ~~Could clock in a byte from audio ADC0804 at byte 89~~  
  * ~~Signals for progressive sync, blank, restart counter, start ADC, latch ADC, latch DAC, gate colorburst~~  
  * ~~AND with a latched bit or a pin output from IO MCU to enable color~~  
  * ~~Set ROM so that start of vblank is start of ROM, so then “reset counter” can be routed to VIDEO\_IRQ, need another latched bit to enable that~~  
* Or go to 8bpp 640x480 @ 25MHz  
  * Probably generate timing and VRAM fetch in a couple of CPLDs at system clock to reduce fetch time  
  * Latch 16 bits at a time  
  * Use an 8-bit-to-R5G6B5 lookup from a couple of 256x8 SRAMs or one 256x16 SRAM  
  * Do audio some other way, I guess  
* Pi Pico 2 W for 2 USB host ports for USB keyboard and mouse, and other fun tricks maybe

There’s a possibility of a DMA-like engine for SD bulk read - basically use the video clock as SPI CK, and use the video address as an SRAM I/O buffer address; very tricky to get right, but could start to approach 7MB/sec if things lined up

Need to buy

* All BOM parts except RAM, ROM, CPU  
  * Later upgrade to 68010 or 68HC000  
  * Later upgrade to 2x 512K for 1MB  
* 12Mhz oscillator  
* Sockets for all RAM, ROM, CPU, GLUE, oscillator  
* FT232H for programing CPLDs  
* PS/2 keyboard and mouse - [this one?](https://www.amazon.com/Perixx-PERIDUO-117P-Wired-Standard-Keyboard/dp/B0D94MSGNN/ref=sr_1_3?dib=eyJ2IjoiMSJ9.LRaxez2V-DpYXp0UCBV0SGJrAaWfAyFnSy3nBKu6lU2RXJ8jcWVSuGXRhg7eOw-LlzjQwTGonOzpmOIRUlO4jboEF7H26-dHbffLu_FfBjingncwnN4dhwBlWajfALYHy5cRK0YODxJTHcMjxZrshLZ_Cr7rwzQw5RKDyVQi8mSikm-0FkQoGJ08SXWYWMnOuitpjsoed0ryTUh9DSe74cYj_tCOqy-fWE48P8dp-CU.40WtxEBchDloWqPv6jQ788EwaTTCnW6wNm0vHkN5Z2M&dib_tag=se&keywords=Ps%2F2+Keyboard&qid=1769668870&sr=8-3)  
* Adafruit 4090

Hm, 640 \* 480 @ 60?

* 512K VRAM - 19-bit counter in CPLD  
* R3G3B2 - direct pass through to DAC  
* monochrome1 bpp - 1-of-8 selection  
  * But then RAM addressing needs to be /8…  
* Need 19 CPU address bits and 8 data bits into VRAM, both gated by VRAM\_SELECT - that’s 4 8-bit buffers  
  * And cannot access except during blanking  
* Note that issues are similar in some ways to palette SRAM except fewer address pins, may be able to gate palette address and data through CPLD  
* Could latch pixel row  
  * 10 bit address latch in CPLD, forces addresses to VRAM to be 20 bits  
* Could there be a RAM row populated in hblank from system RAM? 1Kx8 SRAM, need 640 transfers - have (800 - 640\) \* (1 / 25.2) S = 6.34µS = 76 machine cycles at 12MHz, so **no**

