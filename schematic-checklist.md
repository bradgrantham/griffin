# Rev 2 Schematic Capture Checklist

One component (or component cluster) per box.  Walk each item, work out its
pulls / caps / diodes / termination in discussion, then check it off.
Detail sources: griffin.md "Components" + "Board changes", griffin.yml
`interfaces:` (read the matching entry before capturing each item), and the
CPLD `//PIN:` blocks in the Verilog.  **No pin numbers in this file**: a CPLD
pin lives only in its Verilog `//PIN:` block, and every number copied here
drifted within a month.  Pull-ups, pull-downs, and decoupling/supply caps
deliberately not listed.

## Core

- [x] 68010 CPU (68000 drop-in)
- [x] 14 MHz system oscillator
- [x] 25.175 MHz pixel oscillator
- [x] Optional second-oscillator footprint on PIXEL's free GCLK2 dedicated input (pixel.v header)
- [x] DS1233-5 reset supervisor (button hangs on its RST pin)
- [x] Reset isolation diode: Schottky, anode on ~RESET net, cathode at RST
- [x] nSUPERVISOR_RESET net: DS1233 RST -> GLUE (sources ~HALT)
- [x] Reset button
- [ ] USB-C power entry (stuffing option): 6-pin power-only receptacle, 2x 5.1k Rd, LM339 + TL431 CC gate, 2x NDP6020P back-to-back switch, WEAK_SOURCE LED — griffin.yml "USB-C power entry" - https://claude.ai/code/artifact/b8235433-803e-4c21-8ebb-259b60a3a63f
- [x] 6-pin AMP MATE-N-LOK power connector, Apple II pinout (verify before silk); +12/-12/-5 passed through to a 4-pin drive power connector at the bay - https://www.digikey.com/en/products/detail/te-connectivity-amp-connectors/640500-1/187758
- [ ] Panel barrel-jack harness (5.5 x 2.1 mm centre-positive + inline switch) to the +5 V positions

## Memory

- [x] RAM: 8x AS6C4008 DIP-32, socketed
- [x] ROM: 2x SST39SF040 DIP-32, socketed (new footprint vs the rev-1 W27C512 DIP-28); WE# from GLUE ~ROM_WE, both chips in parallel

## CPLDs (six ATF1508AS PLCC84, one JTAG chain)

- [x] GLUE (spare: the input-only pin 2 to a bodge pad — glue.v header; pin 64 is ~IO_BUFFER_OE since 2026-11-07)
- [x] PORTS (new chip)
- [x] ENGINE (released pins unconnected — engine.v header)
- [x] ENGINE -> GLUE status wires ENGINE_ACTIVE / ENGINE_WAITING, ENGINE -> GLUE ~ENGINE_IRQ, GLUE -> ENGINE ~ENGINE_SELECT
- [x] PIXEL (new chip; replaces VIDEO; TIMING is a module inside it, there is no separate TIMING chip)
- [x] COMPOSITOR (new chip)
- [x] AUDIO (new chip): ~R / ~EF of the five audio FIFOs, the four volume gates, HBLANK from PIXEL
- [x] PIXEL raster fan-out: HBLANK -> ENGINE and AUDIO, nVSYNC_IRQ -> GLUE, PADDLE_TICK -> PORTS, AUDIO_TICK -> test pad, H_ACTIVE -> COMPOSITOR
- [x] PIXEL: DUART OP4..OP7 routed to the four pins reserved in pixel.v's header (no logic yet)
- [x] PIXEL spare-I/O access (header / test points) so spares are bitfile-usable later
- [x] JTAG chain order: GLUE -> PORTS -> AUDIO -> ENGINE -> PIXEL -> COMPOSITOR (AUDIO placed 2026-10-09); links named {earlier}_TO_{later}_TD

## FIFOs (IDT7200, byte wide)

- [x] PIXELS FIFO pair: 2x IDT7200, read as one ganged 16-bit port by PIXEL
- [x] VIDCMD FIFO pair: 2x IDT7200 (new), read by COMPOSITOR
- [x] AUDIO FIFOs: 5x IDT7200 (channels 0,1 on D[15:8]; channels 2,3 and control on D[7:0]), each ~W from its own ENGINE strobe, ~RS from ~RESET, ~HF unconnected
- [x] Video FIFO write buffers: 2x 74AHCT541 (/OE low) with the PIXELS/VIDCMD parts, inputs on D[15:0], outputs D_VIDEO_FIFO[15:0]
- [x] Audio FIFO write buffers: 2x 74AHCT541 (/OE low) with the five audio parts and the AUDIO CPLD, inputs on D[15:0], outputs D_AUDIO_FIFO[15:0]; the main bus does not run to the audio section
- [x] IO data buffers: 2x 74AHCT245 (U49/U50 on the glue sheet; A = D_IO, B = D, DIR = R/~W, /OE = ~IO_BUFFER_OE from GLUE pin 64), 2x 10k SIP-9 pull-up networks on D_IO[15:0], bypass; GLUE, PORTS, DUART and CF data pins relabelled D_IO0..15 (griffin.yml "IO data buffers")
- [x] Series axial footprint (stuffed 0R) at the driver on every 7200 /RE and /W line

## Video output

- [x] 2x 74AC541 video buffers
- [x] R4G4B4 binary-weighted ladder + shunt (3 channels; values in griffin.yml VGA interfaces entry)
- [x] HSYNC/VSYNC series resistors
- [x] DE-15 VGA connector
- [x] Palette RAM: three 16x4 separate-I/O parts (74F219 pinout), IDX[3:0] + nCLUT_WE from PIXEL, data in from COMPOSITOR set_pix_value[11:0], outputs to COMPOSITOR RGB_IN, /CS tied low — drawn with a placeholder 74x219 symbol, **part not chosen** (griffin.yml `issues:`)

## Audio output (griffin.md "Audio"; BOM in griffin.yml audio interfaces entries)

- [x] 4x 4610X-R2R-103LF R2R ladders, straight off the channel FIFOs' Q pins (offset-binary samples: no MSB inverters, no pulls on Q)
- [x] 1x 74HC4066: one volume gate per ladder, driven by AUDIO
- [x] 1x LM358: two inverting summers (L, R) biased at 2.5 V, 2k feedback || 8.2 nF, 4.7k class-A pull-down, 100 uF to the LINE node
- [x] Line out: LINE -> 100R -> 2x RCA jacks (retainer feet)
- [x] Dual-gang 10k audio-taper volume pot across LINE (panel part; bring to a header)
- [x] Headphones: wiper -> 10k / 1k pad -> 2x LM386 (gain 20) -> 10R + 47 nF Zobel -> 250 uF -> switched 3.5 mm stereo jack
- [x] Internal speaker: the jack's NC tip/ring contacts -> 2x 47k sum + 1k -> LM386 -> 10R + 47 nF Zobel -> 250 uF -> 8 ohm speaker (panel part; header); plug insertion mutes  **Jack part open:** SJ3-35063B has a tip switch only (griffin.yml issues); drawn with a generic tip+ring-switch symbol
- [ ] (Analog stage not yet breadboarded — griffin.yml `issues:`)

## Peripherals

- [x] XR68C681 DUART DIP-40, native (retires DIP-carrier bodge); RESET direct on ~RESET net; ~IACK tied high
- [x] 3.6864 MHz DUART crystal
- [x] 2x MAX232 + 1uF charge-pump caps (one per channel: TXD/RXD + RTS/CTS)
- [x] 75189 quad receiver: ch B DCD -> IP4, RI -> IP5 (unused inputs grounded)
- [x] DB-25F console connector, wired DCE; DSR+DCD strapped to DTR at connector
- [x] DB-25M modem connector, wired DTE; DTR-DSR strap (Hayes &D0)
- [x] 4x 1k series resistors on the TTL headers' RXD/CTS lines; 6x 10k pull-ups on DUART RXD_A/B, ~CTS_A/B, IP4, IP5; no isolation jumpers
- [x] MCP7940N RTC (DIP-8) + 32.768 kHz crystal specified for a 6–9 pF load (not the common 12.5 pF part); SCL = OP2, MFP -> IP3
- [x] CR2032 coin-cell holder on VBAT
- [x] 2N7000 for RTC SDA: gate <- OP3, drain on SDA, SDA -> IP2
- [x] CF socket (16-bit True IDE), CFT-125 header at the rear edge
- [x] CF sideband: INTRQ -> GLUE (pull-down), IORDY -> GLUE (pull-up); DMACK to +5
- [x] PS/2 DIN-5 keyboard connector (to GLUE)
- [x] PS/2 DIN-5 mouse connector (to PORTS)
- [x] 2x DE-9 joystick connectors, switch lines straight into PORTS
- [x] Joystick +5V polyfuse
- [x] Joystick ESD series R + clamps
- [x] Paddle FETs: 2x 2N7000 (port 1), gates <- PADDLE_DUMP
- [x] Paddle timing caps: 2x ~10 nF film (DNP-able with the FETs)

## Headers & debug

- [x] Console TTL header (DUART Ch A, FTDI pinout; 1k series on RXD/CTS)
- [x] Second serial TTL header (DUART Ch B; 1k series on RXD/CTS)
- [x] JTAG programming header 1x6 (TCK/TMS/TDI/TDO/5V/GND)
- [x] Debug/LA headers: 2x 2x16 shrouded (gusmanb LogicAnalyzer V2 J2 pinout) — DEBUG_HEADER sheet
- [x] SYSCLK buffered/series-R tap for LA header (never a bare stub on the clock net)
- [x] Debug LED + NPN driver (DEBUG_OUT also blinks ~1.9 Hz while the CPU is halted)
- ~~ENGINE spare-strobe header near the FIFOs: nSIGNAL_SPARE, GND, +5V~~ (gone: griffin.yml "Spare headers"; the pin became a deposit strobe)
- [x] PORTS spare header: its two spare pins (ports.v header), GND
- [x] GLUE bodge pads for its two spare pins (glue.v header)
- [ ] Test header: every inter-IC signal (GND, +5V, D, A, WRITE_LO/HI, selects, nVPA), silk-screened per pin; ground test-point holes

## Remove from rev-1 schematic

- [x] AT89S52 IO MCU (and its crystal/support)
- [x] USB-C BOB header J3 (replaced by the receptacle + CC gate under Core)
- [x] Composite/NTSC jack + NTSC clock provisions (and the two clock-enable lines)
- [x] VIDEO CPLD
- [x] Audio 74HC373 latch (the CPU audio path and the direct-bus region are gone: no 74155 either)
- [x] 2x 74HCT245 joystick buffers
- [x] 2x 74HC590 paddle counters
- [x] Reset RC (R2/C3)
- [x] W27C512 DIP-28 ROM footprints (now SST39SF040 DIP-32)
- [x] Buzzer (replaced by the speaker)
