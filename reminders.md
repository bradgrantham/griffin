# Reminders

Refreshers on fields outside this project's core competence (68000 buses,
CPLDs, firmware).  Each section holds the textbook and datasheet facts behind
values that griffin.yml states without explanation.  Not a design record:
griffin.yml holds the contracts, griffin.log the dated decisions.  Add a
section whenever something has to be explained twice in a session.  The
numbers here are from memory of the standard references; verify against the
datasheet before soldering.

## Audio output stage

Why the values in the griffin.yml "Audio output stage" entry are what they are.

- **Offset binary is the ladder's native code.**  A ladder DAC maps 0x00 to
  0 V and 0xFF to full scale, so midscale (silence) is 0x80; 8-bit WAV uses the
  same code.  Two's complement (0x00 = silence) would need only the MSB
  inverted (rev 1's CD4049 on Q7), and that inverter had to be CMOS-output:
  the MSB is half of full scale, so a TTL-level high (about 3.5 V) would shrink
  it by a quarter exactly at every zero crossing.  Rev 2 stores offset binary
  and drops the inverter; a signed value v is stored as v + 128.
- **Empty FIFO.**  With /R low and the FIFO empty a 7200's Q pins are
  high-impedance (IDT7200 datasheet, Read Enable), so the ladder floats; that
  is harmless because the channel's 74HC4066 is open whenever /EF is asserted.
  Pull resistors were only ever needed to keep the CMOS inverter's input from
  floating, so with the inverter gone there are none.
- **Rate error for common file rates** (step clock 1,573,437.5 Hz, nearest
  PERIOD): 11,025 Hz at PERIOD 142 plays 11,003 Hz (−3.5 cents); 8,000 Hz at
  196 plays 7,987 Hz (−2.8 cents); 22,050 Hz at 70 plays 22,160 Hz (+8.7
  cents, audible against a reference), so resample 22 kHz material offline.
- **R2R ladder source impedance.**  A 10k R2R network looks like a 10k
  Thevenin source.  A 10k resistor from its output to ground therefore forms a
  2:1 divider (0 to 5 V becomes 0 to 2.5 V) with a 5k source impedance.  No
  separate divider is needed.
- **Single-pole RC corner.**  f = 1 / (2 pi R C).  5k and 3.3 nF give about
  9.7 kHz, a little above the Nyquist limit of the 15.7 kHz sample rate.  It is
  placed before the buffer so the buffer's low output impedance does not
  swamp it.
- **LM358 as a unity buffer at 5 V.**  Its output only reaches about 3.5 V on
  a 5 V rail (not rail to rail), which is enough for a 2.5 Vpp signal.  Its
  output stage is class B and has crossover distortion near zero current; a
  4.7k from the output to ground makes it source current all the time
  (class-A bias) and removes that.
- **AC coupling needs a DC return.**  A series capacitor leaves the far side
  floating unless something ties it to ground.  The volume pot element (10k
  across LINE to ground) is that return, which is why it is never removed.
- **Line level.**  Vrms = Vpp / (2 sqrt 2), so 2.5 Vpp is about 0.88 Vrms.
  Consumer line level is roughly 0.3 to 1 Vrms, so the RCA outputs need no
  attenuation and always run at full level.
- **LM386 basics.**  Gain is 20 with pins 1 and 8 open, 200 with a 10 uF
  between them, and never lower than 20.  With a 5 V supply the output can
  manage about 1.6 Vrms, so the input must be padded: 0.88 Vrms times 20
  would clip, hence the 10k/1k (about 11:1) pad ahead of the headphone amps.
  The datasheet output network is a 10R + 47 nF series RC (Zobel, or snubber)
  from output to ground for stability, plus a large output coupling cap
  (250 uF into 8 ohm gives a corner near 80 Hz) because the output idles at
  half the supply.
- **Pad with two summed sources.**  Two 47k resistors feeding one node act as
  a 47k || 47k = 23.5k source.  Into 1k to ground that is 1k / 24.5k, about
  25:1.  So 1.6 Vrms from a headphone amp becomes about 65 mV, which the
  speaker amp's gain of 20 turns into about 1.3 Vrms.
- **Power.**  P = Vrms squared over R.  1.6 Vrms into 32 ohm is 80 mW
  (headphones); 1.3 Vrms into 8 ohm is about 200 mW (speaker).
- **Pots and jacks.**  "Audio taper" means logarithmic, which is what a
  volume knob wants; "dual-gang" means one shaft, two elements, one per
  channel.  A switched 3.5 mm jack has normally-closed contacts that open
  when a plug goes in, which is what mutes the internal speaker (the Apple IIc
  convention).

## RS-232

Why the griffin.yml "RS-232 level stage" entry is wired the way it is.

- **Levels.**  On the wire, mark (idle, logic 1) is negative (-3 to -15 V)
  and space (logic 0) is positive.  A TTL UART idles high.  A MAX232 driver
  inverts, so TTL high becomes negative, which is correct; its receiver
  inverts back.  Control lines are "asserted" when positive, and the DUART's
  ~RTS pin is low when asserted, so the inverting driver lands that right
  too.  Everything comes out correct with inverting stages; never add an
  extra inverter.
- **MAX232.**  Two drivers and two receivers per chip, exactly TXD/RXD plus
  RTS/CTS for one channel.  Its charge pump makes about +/-10 V from the 5 V
  rail using four external capacitors: 1 uF on the original MAX232, 0.1 uF
  on the MAX232A, so check which variant is in hand.
- **75189 (MC1489).**  Quad RS-232 receiver, 5 V only, no drivers.  Ground
  unused inputs.
- **DTE and DCE.**  A DTE (terminal, computer) transmits on DB-25 pin 2 and
  receives on pin 3.  A DCE (modem) does the opposite.  A straight cable
  connects a DTE to a DCE.  Two DTEs need a null modem cable, which crosses
  2/3, 4/5 and 6/20.  Griffin's console (ch A) is wired as DCE so a terminal
  plugs straight in; the modem port (ch B) is wired as DTE so a modem plugs
  straight in and a PC needs a null modem.
- **DB-25 pins.**  1 shield, 2 TXD (from DTE), 3 RXD, 4 RTS, 5 CTS, 6 DSR,
  7 signal ground, 8 DCD, 20 DTR, 22 RI.  DE-9 for comparison: 1 DCD, 2 RXD,
  3 TXD, 4 DTR, 5 ground, 6 DSR, 7 RTS, 8 CTS, 9 RI.
- **Handshake lines.**  RTS/CTS is hardware flow control.  DTR/DSR say "I am
  here" in each direction.  DCD says the modem has a carrier, RI that the
  line is ringing.  Strapping DTR to DSR (and DCD) at the connector makes the
  other side see "always ready" without Griffin driving a pin.
- **Hayes modems.**  Because Griffin does not drive DTR, the modem must be
  told to ignore it: AT&D0.  Hang up by sending the escape sequence "+++",
  pausing, then "ATH".
- **68681 input port.**  Change-of-state interrupts exist only for IP0 to
  IP3, so IP4 and IP5 (DCD_B, RI_B) are polled.

- **TTL bench header versus MAX232.**  A DUART output (TXD, RTS) can feed
  a MAX232 driver input and a header cable's RXD at the same time; the
  unused one just listens.  A DUART input (RXD, CTS, DCD, RI) must have
  exactly one driver, and a stuffed MAX232 is never quiet: its RS-232
  input has an internal pull-down, so with nothing on the DB-25 its
  receiver output sits high and fights every low a header cable sends.
  Griffin resolves this without jumpers: bench builds leave the MAX232s
  unstuffed, so the header is the only driver; 10k pull-ups keep the
  DUART inputs at idle mark (a floating 68681 RXD receives garbage and
  breaks, floating CTS inputs fire change-of-state IRQs); and 1k series
  resistors in the headers' RXD/CTS lines make a collision harmless if a
  cable is ever left in while a stuffed MAX232 port is live, the MAX232
  winning through the resistor at about 5 mA.  Level translation into an
  FTDI is a separate matter on the other direction (DUART TXD, CPLD TDO).

## USB-C power

Why the griffin.yml "USB-C power entry" gate is built the way it is.

- **Who advertises what.**  The source, not the sink, sets the current
  allowance.  Each CC pin at the source has a pull-up Rp; the sink has a
  5.1k pull-down Rd on both of its CC pins.  Rd only says "a sink is here";
  no sink-side resistor asks for more current.  The sink is expected to read
  the CC voltage the Rp/Rd divider produces and stay under the allowance.
- **The CC voltages.**  Rp 56k to 5 V (or 36k to 3.3 V, or 80 uA): about
  0.41 V, default USB power (500 mA, 900 mA on USB 3).  Rp 22k to 5 V (12k,
  180 uA): about 0.92 V, 1.5 A.  Rp 10k to 5 V (4.7k, 330 uA): about 1.68 V,
  3 A.  The spec bands are 0.25 to 0.61 V, 0.70 to 1.16 V and 1.31 to 2.04 V,
  and the sink decision thresholds between them are 0.66 V and 1.23 V.
  Brad's bench brick reads 1.69 V on CC2, the 3 A advertisement.  Any
  USB-C wall charger of 15 W or more advertises 3 A at 5 V; a computer's
  port may advertise only default.
- **Only one CC is live.**  A cable carries one CC wire, so the sink sees
  the Rp voltage on CC1 or CC2 depending on plug orientation and near 0 V
  on the other (the plug's VCONN pin, possibly with an e-marker's Ra to
  ground).  Hence one comparator per CC pin, outputs ORed.
- **No Rd, no power.**  A source enables VBUS only after it detects Rd, so
  leaving the two 5.1k off makes the socket dead.  That is the display
  build's off switch.
- **A sink must never drive VBUS.**  If the board is powered from the
  harness with a cable plugged in, anything that lets +5V_IN reach VBUS
  violates that and confuses the source's attach detection.  A single P-FET
  cannot block it: its body diode conducts drain to source.  Two P-FETs
  with sources tied put both body diodes nose to nose and block both ways
  when off; tie the gates and drive them together.  The gate pull-up goes
  to that common-source node so that with VBUS absent Vgs is zero and the
  pair stays off.
- **Why not a Schottky.**  A series Schottky blocks backfeed but drops
  0.3 to 0.4 V at 1.7 A, which lands the rail under the DS1233-5 trip
  window and the 68000's 4.75 V minimum.  Two logic-level NDP6020P at
  roughly 50 mOhm each drop about 0.17 V; measure it.
- **Overdraw.**  Pulling more than the advertisement damages nothing on the
  board.  A charger current-limits (the rail sags and the DS1233-5 reset
  loops) or hiccups; a host port trips its per-port overcurrent protection
  and goes dead until re-plugged.  The gate turns that into a clean "won't
  start, LED says why".
- **Inrush.**  The bulk capacitance looks like a short at plug-in.  The
  100 nF on the FET gates against the 100k pull-up ramps the switch on over
  a few milliseconds.  Keep capacitors off CC1 and CC2 themselves; the spec
  limits sink capacitance there.
- **What a comparator is.**  An op-amp-shaped part with no feedback: the
  output snaps low when the (-) input is above the (+) input and floats
  otherwise.  The LM339 is a quad in a DIP-14 with open-collector outputs,
  so they need a pull-up and can be wired together for a free OR (any one
  pulling low wins).  Inputs work down to ground, which the 0 to 2 V CC
  levels need.  Unused sections: tie (+) to ground and (-) to the supply.
- **Reference.**  1.25 V must not come from a VBUS divider, since Rp may be
  to 3.3 V or a current source and VBUS itself may be sagging.  A TL431
  (TO-92, 1k from VBUS, cathode tied to ref) gives 2.495 V; a 10k/10k
  divider halves it.

## RTC over the DUART (I²C by hand)

The MCP7940N hangs on four DUART general-purpose pins; there is no I²C
peripheral.  Two inversions stack up and both are easy to get backwards:

* **OPR complement.**  Writing a 1 to an OPR bit (via OPR_SET) drives that
  OP pin **LOW**; OPR_CLR drives it HIGH.  OPR resets to 0, so all OP pins
  come up HIGH.
* **The SDA FET.**  OP3 drives the gate of a 2N7000 whose drain is SDA.  OP3
  HIGH turns the FET on and pulls SDA LOW; OP3 LOW releases SDA to its
  pull-up.  So SDA released = OPR bit 3 **set** (pin low), SDA driven low =
  OPR bit 3 clear.
* SCL is OP2 directly: SCL high = OPR bit 2 clear.  Push-pull is fine
  because the MCP7940N never stretches the clock.
* Consequence: from reset until firmware sets OPR bit 3, SDA is held LOW.
  Harmless to the chip (it sees a START with no clocks), but `rtc_init`
  clocks SCL nine times with SDA released and sends a STOP first, in case
  the reset interrupted a read and the chip was mid-byte with SDA low.
* IP2 reads SDA back; IP3 reads MFP.  The I²C bit rate is one DUART bus
  write per SCL edge, which is well inside the chip's 400 kHz fast-mode
  limits (tLOW ≥ 1.3 µs, tHIGH ≥ 0.6 µs) even with a few NOPs of padding.

Wall time does not come from I²C after boot.  MFP is programmed as a 1 Hz
square wave; the DUART raises its level-5 interrupt on any IP0–IP3 change
whose ACR enable bit is set (ACR[3] for IP3), ISR bit 7 (IPC) says so, and
reading IPCR both reports the delta bits (bits 7:4) with the current levels
(bits 3:0) and clears the deltas.  The ISR increments `rtc_seconds` only when
IPCR shows delta-IP3 AND IP3 high, i.e. the rising edge; the falling edge
costs an interrupt and nothing else.

Chip facts the driver relies on: register 0x00 bit 7 is ST (oscillator
start), register 0x03 bit 3 is VBATEN (battery switchover enable; off on a
fresh part), bit 5 is OSCRUN (read-only); register 0x07 is CONTROL (bit 6
SQWEN, bits 1:0 SQWFS, 00 = 1 Hz); register 0x08 OSCTRIM.  Two-digit year,
read as 2000–2099.  Crystal must be a 6–9 pF load part, not the common
12.5 pF one.  The datasheet asks for ST cleared while the time registers are
written and set again afterwards; `rtc_write_time` does that.
