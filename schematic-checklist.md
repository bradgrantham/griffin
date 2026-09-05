# Rev 2 Schematic Capture Checklist

One component (or component cluster) per box.  Walk each item, work out its
pulls / caps / diodes / termination in discussion, then check it off.
Detail sources: griffin.md "Rev 2 components" + "Board changes",
griffin.yml `interfaces:` (read the matching entry before capturing each
item), CPLD `//PIN:` blocks in the Verilog.
Pull-ups, pull-downs, and decoupling/supply caps deliberately not listed.

## Core

- [x] 68010 CPU
- [x] 14 MHz system oscillator
- [x] 25.175 MHz pixel oscillator
- [x] Optional second-oscillator footprint (TIMING GCLK2, pin 44)
- [x] DS1233-5 reset supervisor (button hangs on its RST pin)
- [x] Reset isolation diode: Schottky, anode on ~RESET net, cathode at RST
- [ ] nSUPERVISOR_RESET net: DS1233 RST -> GLUE pin 84 (sources ~HALT)
- [x] Reset button
- [x] 6-pin power header - https://www.digikey.com/en/products/detail/te-connectivity-amp-connectors/640500-1/187758?fbclid=IwY2xjawUG6vZwZG9mAWV4dG4DYWVtAjEwAGJyaWQRMVMxVzh6bW9WUkdEcHlBaktzcnRjBmFwcF9pZBAyMjIwMzkxNzg4MjAwODkyAAEeWl4eztEJP8iR58QZ9OmAJ92TIHq7s3BUc4BR4RiXnqa1eGCCplmakONtw0Q_aem_Bxny1SMMt7ad0dHB6OfFug

## Memory

- [x] RAM: 8x AS6C4008 DIP-32, socketed
- [x] ROM: 2x SST39SF040 DIP-32, socketed (new footprint vs W27C512)

## CPLDs

- [x] GLUE ATF1508 PLCC84 (rev-2 pinout re-frozen 2026-09-04; spares 64 (I/O) and 2 (input-only) to bodge pads)
- [x] PORTS ATF1508 PLCC84 (new chip)
- [ ] ENGINE ATF1508 PLCC84 (pins 40/41/46/81 released, unconnected)
- [ ] ENGINE -> GLUE status wires: ENGINE_ACTIVE 80->35, ENGINE_WAITING 4->36
- [ ] PIXEL ATF1508 PLCC84 (new chip; replaces VIDEO)
- [ ] COMPOSITOR ATF1508 PLCC84 (new chip)
- [ ] TIMING ATF1504AS PLCC44 (new chip)
- [ ] TIMING spare-I/O access (header / test points for the 13 spares)

## FIFOs

- [ ] PIXELS FIFO pair: 2x IDT7200
- [ ] VIDCMD FIFO pair: 2x IDT7200 (new)
- [ ] AUDIO FIFO pair: 2x IDT7200

## Video output

- [ ] 2x 74AC541 video buffers
- [ ] R4G4B4 weighted-resistor DAC (3 channels)
- [ ] HSYNC/VSYNC series resistors
- [ ] DE-15 VGA connector

## Audio output

- [ ] 2x 4610X-R2R-103LF R2R networks
- [ ] CD4049 MSB inverters (2 of 6 gates; CMOS-output required, unused inputs grounded)
- [ ] LM358 dual buffer + per-channel 10k divider / 3.3nF pole / 4.7k class-A pull-down / 100uF + 100R output network (values in griffin.yml audio interfaces entry)
- [ ] Dual-gang 10k audio-taper volume pot (panel mount + knob; the pot is the DC return)
- [ ] LM386 + 8-ohm internal speaker, fed from the jack's NC switch contacts (2x 10k sum + 47k; 10R + 47nF Zobel; 250uF out) -- plug insertion mutes; rev-1 buzzer retired
- [ ] Stereo jack, switched (tip + ring NC contacts)

## Peripherals

- [ ] XR68C681 DUART DIP-40, native (retires DIP-carrier bodge); RESET direct on ~RESET net
- [ ] 3.6864 MHz DUART crystal
- [ ] 2x MAX232 + 1uF charge-pump caps (one per channel: TXD/RXD + RTS/CTS)
- [ ] 75189 quad receiver: ch B DCD -> IP4, RI -> IP5 (unused inputs grounded)
- [ ] DB-25F console connector, wired DCE; DSR+DCD strapped to DTR at connector
- [ ] DB-25M modem connector, wired DTE; DTR-DSR strap (Hayes &D0)
- [ ] 4x isolation jumpers on MAX232 receiver outputs (RXD/CTS, both channels)
- [ ] 74HCT155 direct-bus decoder
- [ ] DS3231 RTC
- [ ] Coin-cell holder
- [ ] 2N7000 for RTC SDA
- [ ] CF socket (16-bit True IDE)
- [ ] CF sideband: INTRQ -> GLUE 40 (pull-down), IORDY -> GLUE 2 (pull-up)
- [ ] PS/2 keyboard connector (to GLUE)
- [ ] PS/2 mouse connector (to PORTS)
- [ ] 2x DE-9 joystick connectors
- [ ] Joystick +5V polyfuse
- [ ] Joystick ESD series R + clamps
- [ ] Paddle FETs: 2x 2N7000 (port 1)
- [ ] Paddle timing caps: 2x ~10 nF film

## Headers & debug

- [ ] Console TTL header (DUART Ch A, FTDI pinout; behind isolation jumpers)
- [ ] Second serial TTL header (DUART Ch B; behind isolation jumpers)
- [ ] JTAG programming header 1x6
- [ ] Debug/LA headers: 2x 2x15 (gusmanb pinout)
- [ ] SYSCLK buffered/series-R tap for LA header
- [ ] Debug LED(s) (DEBUG_OUT also blinks ~1.9 Hz while CPU halted)
- [ ] ENGINE spare-strobe header near FIFOs: nSIGNAL_SPARE (pin 9), GND, +5V
- [ ] PORTS spare header: pins 34, 41, GND
- [ ] GLUE bodge pads: pins 64 (I/O) and 2 (input-only, OE1/IN) (unassigned)
- [x] MATE-N-LOK 4-pin Molex power male

## Remove from rev-1 schematic

- [ ] AT89S52 IO MCU (and its crystal/support)
- [x] USB-C power entry
- [x] Composite/NTSC jack + NTSC clock provisions
- [x] VIDEO CPLD
- [ ] Audio 74HC373 latch
- [ ] 2x 74HCT245 joystick buffers
- [ ] 2x 74HC590 paddle counters
- [ ] Reset RC (R2/C3)
