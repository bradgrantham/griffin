# Rev 2 Schematic Capture Checklist

One component (or component cluster) per box.  Walk each item, work out its
pulls / caps / diodes / termination in discussion, then check it off.
Detail sources: griffin.md "Rev 2 components" + "Board changes",
griffin.yml `interfaces:` (read the matching entry before capturing each
item), CPLD `//PIN:` blocks in the Verilog.
Pull-ups, pull-downs, and decoupling/supply caps deliberately not listed.

## Core

- [ ] 68010 CPU
- [ ] 14 MHz system oscillator
- [ ] 25.175 MHz pixel oscillator
- [ ] Optional second-oscillator footprint (TIMING GCLK2, pin 44)
- [ ] DS1233-5 reset supervisor (button hangs on its RST pin)
- [ ] Reset isolation diode: Schottky, anode on ~RESET net, cathode at RST
- [ ] nSUPERVISOR_RESET net: DS1233 RST -> GLUE pin 63 (sources ~HALT)
- [ ] Reset button
- [ ] Barrel jack power entry (5.5mm x 2.1mm center-positive)
- [ ] Inline power switch
- [ ] Reverse-polarity protection (P-FET ideal-diode)

## Memory

- [ ] RAM: 8x AS6C4008 DIP-32, socketed
- [ ] ROM: 2x SST39SF040 DIP-32, socketed (new footprint vs W27C512)

## CPLDs

- [ ] GLUE ATF1508 PLCC84 (new rev-2 pinout; spare I/O 24, 64 to bodge pads)
- [ ] ENGINE ATF1508 PLCC84 (pins 40/41/46/81 released, unconnected)
- [ ] ENGINE -> GLUE status wires: ENGINE_ACTIVE 80->35, ENGINE_WAITING 4->36
- [ ] PORTS ATF1508 PLCC84 (new chip)
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

- [ ] Console TTL header (DUART Ch A, FTDI pinout)
- [ ] Second serial TTL header (DUART Ch B)
- [ ] JTAG programming header 1x6
- [ ] Debug/LA headers: 2x 2x15 (gusmanb pinout)
- [ ] SYSCLK buffered/series-R tap for LA header
- [ ] Debug LED(s) (DEBUG_OUT also blinks ~1.9 Hz while CPU halted)
- [ ] ENGINE spare-strobe header near FIFOs: nSIGNAL_SPARE (pin 9), GND, +5V
- [ ] PORTS spare header: pins 34, 41, GND
- [ ] GLUE bodge pads: pins 24, 64 (unassigned)

## Remove from rev-1 schematic

- [ ] AT89S52 IO MCU (and its crystal/support)
- [ ] USB-C power entry
- [ ] Composite/NTSC jack + NTSC clock provisions
- [ ] VIDEO CPLD
- [ ] Audio 74HC373 latch
- [ ] 2x 74HCT245 joystick buffers
- [ ] 2x 74HC590 paddle counters
- [ ] Reset RC (R2/C3)
