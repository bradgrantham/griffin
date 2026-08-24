# Rev 2 pre-schematic electrical review

2026-08-15.  Scope: signaling, level, and current issues to nail down before
KiCad capture.  Sources: griffin.yml (peripherals + interfaces), griffin.md
(Rev 2 components / Board changes), board-pcb-rev-1.distilled.txt (empirical
rev-1 wiring), cpld/glue/glue.v and cpld/engine/engine.v (drive styles),
chip-inventory.csv.  Numbers marked **[verify]** are stated from recollection
and must be pinned from the datasheet table before they are relied on
(feedback_timing_discipline); everything else is either read from a project
file or counted from the rev-1 netlist.

## 0. Summary of findings

| # | Finding | Action |
|---|---------|--------|
| 1 | CPU bus needs no transceiver (settled earlier today) | none — drop the "7200s may need '245s" watch item |
| 2 | Plain-TTL 74155 loads A18/A19 with mA-scale IIL, and burns ~40 mA ICC | swap to 74HCT155 (user agreed) |
| 3 | Rev 1 never had the DS1233 — reset was RC + button.  RESOLVED 2026-08-16: DS1233D-5 on hand, datasheet pinned; note the D variant has **no pushbutton detect** (§3) | accepted; bring-up gate: rail ≥4.85 V at the supervisor |
| 4 | FIFO /RE gate timing table still unpinned; it decides 74AC00 vs 74F00 vs the 2-slot fallback — a BOM + netlist difference | gating item: pin the table before capture |
| 5 | Power: rev 2 adds ~0.5–0.8 A over rev 1's measured 0.84 A.  RESOLVED 2026-08-16: input standardized on a 5.5×2.1 mm center-positive barrel jack, regulated 5 V ≥2.5 A, silkscreened contract (USB-C dropped) | budget with datasheet ICC; size jack/switch/traces ≥2 A |
| 6 | Audio output stage: R2R full scale 5 V > LM358 VOH ceiling (~3.5 V at 5 V rail) — top of scale clips | divider before the buffer (§7) |
| 7 | Pull-up schedule: several nets need pull-ups that rev 1 lacked, and the AS/UDS/LDS/R-W set is load-bearing on **every ENGINE DMA handover**, not just debug mode | schedule in §2 |
| 8 | CPU RESET instruction resets GLUE → CONFIG clears → ROM overlay returns under a running CPU | firmware rule: never execute `reset` (document; no hardware fix) |
| 9 | Bin "68681" ×3 are MC parts — not drop-ins for the XR68C681 (firmware 115200 init is XR-specific extend-command sequence) | label as spares-at-low-baud only |
| 10 | Several quantities/parts unconfirmed in bin: 6× IDT7200, ATF1504AS-PLCC44, SST39SF040 ×2, 74AC541 ×2, 74AC00, DS3231, 2N7000, polyfuses | procurement check (§9) |

---

## 1. CPU bus (fan-out, drive, levels) — reviewed earlier today, recap

No transceivers.  Rev-1 counted loads: A1 = 16 pins, D0 = 12 pins (plus the
bodged FIFO piggybacks physically present), and that bus runs at 14 MHz today.
Rev-2 worst-case data line is ~14 loads (adds PORTS, one 7200 per stream per
lane; drops VIDEO and the '373) ≈ 110–140 pF against the 68000's 130 pF AC
test-load condition **[verify test load from MC68000/68010 datasheet]**.  DC
loading is µA-scale CMOS everywhere once the 74155 → 74HCT155 swap is made.
All 68000 timing margins that matter (0-WS RAM ~90–100 ns address-to-data
slack over 55 ns tAA; ROM 1 WS; CF 7 WS) absorb the residual edge-slew.

Remaining bus items:

- **ENGINE as master**: ATF1508AS output timing is characterized at ~50 pF
  **[verify]**; it drives the same ~130 pF bus during DMA.  The 2-cycle/word
  budget (~143 ns) should absorb the derating — confirm against the tCO table
  and whatever load-derating note the datasheet gives.
- **Level check sweep** (cheap, do once): NMOS 68000 VOH min is 2.4 V at
  −400 µA **[verify]**.  Every bus input must accept that as high:
  AS6C4008 VIH 2.2 **[verify]**, SST39SF040 VIH 2.0 **[verify]**, ATF1508AS
  VIH 2.0 (TTL-compatible), XR68C681 **[verify]**, IDT7200 **[verify]**,
  CF card at 5 V **[verify — CF spec VIH; tightest entry in the sweep]**,
  74HCT155 VIH 2.0.  Real-world NMOS VOH into pure-CMOS loads sits near 4 V,
  and rev 1 works, so this is a paper sweep, not a redesign risk.
- **No AC-family input may sit on the CPU bus.**  74AC VIH is 0.7·VCC = 3.5 V.
  Checked by construction: the two 74AC541s and the 74AC00 see only
  CPLD/oscillator rail-swing outputs.  Keep it that way if gates get added.

## 2. Pull-up schedule (the capture-time net rules)

Verified drive styles: glue.v drives ~DTACK, ~BERR, ~VPA, IPL2:0 totem-pole;
~HALT open-drain (`RESET ? 0 : z`); PS/2 pins open-drain.  engine.v drives
~BR/~BGACK totem-pole always, A/D/AS/UDS/LDS only while BGACK asserted.
Consequences:

| Net(s) | Value | Why |
|---|---|---|
| ~AS, ~UDS, ~LDS, R/~W | 4.7–10 K | **Load-bearing on every DMA burst**: between CPU release (BGACK) and ENGINE drive, and again at burst end, these float.  A floating ~AS into GLUE = phantom cycle.  Also needed for debug-header master mode.  Rev 1 had none of these. |
| ~DTACK, ~BERR, ~VPA, ~IPL2:0 | 10 K | GLUE drives them totem-pole, so these only matter when GLUE is blank or mid-JTAG — but floating IPL lines drift low = phantom **level-7 NMI** (unmaskable).  Cheap insurance, 7 resistors. |
| ~BR, ~BGACK | 10 K (replaces rev-1 100 K) | ENGINE drives totem-pole; pull-up covers blank-CPLD only.  100 K worked; 10 K standardizes the BOM. |
| ~ENGINE_IRQ, ~DUART_IRQ, ~PORTS_IRQ | 10 K | Already on the md list.  XR68C681 IRQN is open-drain **[verify]** so its pull-up is functional, not insurance. |
| ~DUART_DTACK | 10 K | **[verify XR68C681 DTACKN output structure]** — if open-drain (MC68681 heritage suggests so), GLUE would otherwise sample a floating line.  Pull-up is correct in either case. |
| ~ROM_WE | 10 K | Flash WE# idles high while GLUE is blank/JTAG.  SST SDP already blocks stray programming (full JEDEC unlock required), so this is belt + suspenders. |
| ~HALT | 4.7 K | Bodge #3 promoted to design (already listed). |
| ~RESET | 10 K | Wire-OR net: DS1233 + button + CPU (bidirectional!) + GLUE input + CPLD GCLRs + CF RESET + DUART via GLUE's buffered copy. |
| PS/2 CLK/DATA ×2 ports | 4.7 K to +5 | Already listed. |
| I²C SDA | 4.7 K | SCL is push-pull from OP2 — no pull-up needed there (harmless if added). |
| JTAG | TMS/TDI 10 K up; TCK pull-down ~1 K **[verify against Atmel ATF15xx ISP app note]** | One set for the 5-device chain. |
| Joystick switch lines | bussed 4.7–10 K networks; **1 MΩ discretes on paddle-port pins 5/9 only** | Already designed. |

Deliberately no pull-ups: chip selects and WRITE_LO/HI (blank-GLUE scribbles
into RAM are harmless), nVSYNC (VSYNC_IRQ_EN gates the only consequence).

## 3. Reset — RESOLVED 2026-08-16 (DS1233D-5 on hand, datasheet 041002 pinned)

- **Rev-1 truth**: netlist shows ~RESET = 10 K pull-up + cap + button +
  CPU + GLUE + ENGINE/VIDEO GCLR + CF.  **No DS1233 anywhere on the board.**
  The supervisor is a new rev-2 part; drop the bare RC (the supervisor
  provides the ~350 ms POR delay; a fat cap directly on the net would fight
  the CPU's own 124-clock RESET-instruction drive and slow all edges).
- **Part on hand: DS1233D-5.**  From the datasheet: open-drain RST with
  internal 5 kΩ (3.75–6.25 k) pull-up, trip 4.50–4.74 V (typ 4.625 V),
  250–450 ms stretch, IOL 8 mA — wire-OR compatible with the button, the
  CPU, and the debug-header master.  **The D variant has no pushbutton
  detect** (selection guide: N/A vs the plain DS1233's 2.4–3.3 V detect):
  it tolerates an external low but does not debounce or re-stretch it, so
  the button's release rides raw switch bounce.  Accepted: the bounce
  window sits immediately after a completed reset, worst case is a botched
  boot cured by pressing again; the plain DS1233-5 is a drop-in upgrade if
  it ever annoys.
- **Trip margin**: 4.74 V worst-case trip is the tight variant, viable
  because the barrel-jack supply (§8) has no USB cable-drop stack.
  Bring-up gate: measure ≥4.85 V at the supervisor pin under full load;
  if not, move to DS1233D-10 (4.25–4.49 V) or trim the supply.  Note the
  tight trip is also *protective*: below 4.75 V the NMOS CPU and friends
  are out of their 5 V ±5 % spec anyway.
- **Power-on sequencing**: 68000 wants ~RESET *and* ~HALT low ≥100 ms at
  power-up.  DS1233 holds RESET; GLUE converts that to ~HALT
  (open-drain, verified in glue.v).  GLUE is an instant-on EEPROM CPLD and
  configures in ≪ the 350 ms window **[verify power-up-to-operational
  figure]**; until then the 4.7 K holds ~HALT high, which is fine because
  RESET is still asserted when GLUE comes alive.
- **Behavioral trap (no schematic fix)**: the CPU's `reset` instruction
  drives ~RESET low for 124 clocks *without resetting the CPU*.  Our net
  resets GLUE → CONFIG clears → ROM overlay re-enables and FLASH_WE_EN
  drops, under a still-running CPU whose PC is in mapped-RAM space.  Rule:
  **firmware must never execute `reset`**; peripheral resets happen via
  their registers.  Document in griffin.yml; consider a startup assert in
  code review culture rather than hardware.
- CF RESET stays on the system net (as rev 1).  DUART gets GLUE's buffered
  totem-pole copy (nDUART_RESET, already in glue.v).

## 4. Clocks

- **14 MHz SYSCLK**: oscillator → CPU + GLUE + ENGINE + PORTS (4 loads).
  33 Ω series at the oscillator, single daisy-chained trace, no stubs; each
  CPLD takes it on a GCLK pin (already required).  The LA-header tap is
  buffered/series-R per the existing note — never stub the raw net.
  **[verify: 68000 CLK input VIH/VIL — the clock input spec is sometimes
  tighter than TTL on NMOS CPUs; a full-swing HCMOS can oscillator makes it
  moot, which is what Y1 is]**.
- **25.175 MHz pixel clock**: oscillator → TIMING + PIXEL + COMPOSITOR +
  74AC00 (4 loads).  Same treatment.  The three video CPLDs exchange
  same-clock synchronous handshakes at 39.7 ns period, so keep their clock
  arrivals close: one short daisy trace, inches apart — CPLD-internal clock
  paths dominate skew after that.
- **3.6864 MHz DUART crystal** + 2× 33 pF, tight to the chip (carried from
  the rev-1 carrier design).
- **Delete Y4 (12 MHz)** — it clocked the AT89S51, which is gone.
- Inter-CPLD synchronous buses (TIMING strobes, PIXEL→COMPOSITOR RGB,
  COMPOSITOR→PIXEL SET bus): full-cycle setup at 39.7 ns with ~7.5 ns tCO
  **[verify]** + ~3 ns tSU **[verify]** leaves >25 ns for routing — ordinary
  traces, no matching beyond "same board region".

## 5. Compact Flash

Carryover fixes already tracked (16-bit, IOWR AS-gate — already in glue.v,
CS0/CS1 swap native, DMACK→+5, redo the junk symbol).  Additional capture
notes:

- CSEL → GND (master), ~ATASEL/OE grounded for True IDE at power-up
  **[verify pin name/polarity against the CF spec sheet used for the
  symbol redo]**.
- IORDY from the card: unused, leave NC (PIO with fixed 7 WS).
- Level sweep entry from §1 applies (CF VIH at 5 V is the tightest check).
- Hot-plug policy: none.  Power down to swap cards; no isolation buffers.
  (Rev 1 ran the slot direct and it's fine as a policy.)
- Card current: 5 V CF cards spec up to ~100 mA, some to 200 mA on writes
  **[verify against the card actually used]** — feeds the §8 budget.

## 6. PS/2, joysticks, paddles

- PS/2: both ports open-drain from the CPLDs (verified style in glue.v;
  PORTS mirrors it), 4.7 K pull-ups, +5 via polyfuse.  Devices can draw
  100–275 mA each (some keyboards) **[verify chosen polyfuse hold current
  covers keyboard + mouse]**.  Optional 100 Ω series + clamps at the
  connector per the existing note.
- Joysticks: switch lines straight into PORTS with bussed pull-ups — fine;
  the ESD boundary is the series R + clamps called out in Board changes.
- Paddles: pot ramp (1 MΩ pot ∥ 1 MΩ pull-up into ~10 nF) is sensed by a
  raw ATF1508 input used as the comparator.  Two consequences to accept
  knowingly: (a) millisecond-scale slew through the input threshold — the
  2-FF synchronizer (mandatory per project rule) makes this safe logically;
  threshold chatter just jitters the count by a line or two, absorbed by the
  already-planned software calibration; (b) slow-slewing inputs raise CMOS
  input-stage current a little — negligible at two pins.  No LM358 in the
  paddle path in rev 2 (it moves to audio only).
- 2N7002 is SOT-23.  The board is THT-by-intent: use **2N7000 (TO-92)** for
  the two paddle dump FETs and the RTC SDA pull-down, or accept three SMD
  pads.  3 needed.

## 7. Audio output stage (the "redesign analog near the jack" item, made concrete)

Chain: 7200 Q[7:0] per channel → R2R ladder → divider → LM358 half → AC
couple → jack.

- 7200 outputs drive the 10 K-class R2R directly — trivial load, fine.
- **Finding**: R2R full scale ≈ 5 V, but LM358 output ceiling at a 5 V rail
  is ~VCC−1.5 ≈ 3.5 V **[verify VOH from LM358 table]**.  A unity buffer
  clips the top ~30 % of codes.  Fix: divide the ladder output ~2:1 (two
  equal resistors) → 0–2.5 V, then LM358 unity → ~2.5 Vpp, right at line
  level (~1 Vrms) after the coupling cap.
- LM358 crossover distortion: bias each output into class A with a 4.7–10 K
  pull-down to GND at the op-amp output, before the coupling cap.
- Output network per channel: 100 Ω series + 220 µF AC coupling (rev-1
  values carried), ladder + op-amp placed at the jack per the existing
  layout note.
- LM358 source: U15 on the rev-1 board, or a fresh one — not in the bin CSV.

## 8. Power budget and input path

Rev 1 measured **0.84 A** on a plain 5 V sink (griffin.md).  Rev 2 deltas,
typ estimates — **every ICC row here is [verify from datasheet]**:

| Change | Est. delta |
|---|---|
| +1 ATF1508AS net (PORTS; VIDEO out, PIXEL+COMPOSITOR in = +1) | +100–170 mA each; ATF1508AS ICC dominates the whole board (4 chips) |
| +1 ATF1504AS (TIMING) | +70–100 mA |
| +4 IDT7200 net (6 total vs 2 bodged) | +120–320 mA (family/suffix-dependent — check what "7200 LP15" on hand actually is; LP is the low-power grade) |
| SST39SF040 ×2 replace W27C512 ×2 | ≈ wash |
| XR68C681 native (CMOS) | ≈ wash (~15 mA) |
| 74155 → 74HCT155 | −40 mA (std-TTL ICC eliminated) |
| 74AC541 ×2, 74AC00, VGA DAC load (~30 mA peak white), keyboard+mouse (100–300 mA), CF card (50–200 mA on writes) | +200–500 mA situational |

Ballpark total: **1.3–1.7 A typ, ~2.2 A worst-case simultaneous** — treat as
an estimate to be replaced by a datasheet-sourced table before ordering the
supply path parts.

- **Input path — RESOLVED 2026-08-16**: 5.5×2.1 mm center-positive barrel
  jack, regulated 5 V ≥2.5 A adapter, inline switch kept; contract on the
  silkscreen at the jack.  (USB-C dropped: a plain-Rd sink is only entitled
  to 500 mA without CC sensing or PD; a CH224K-style trigger module with a
  barrel pigtail adapts any PD charger if ever needed.)  Still applies:
  rate the jack/switch/input traces ≥2 A; consider a 2 A polyfuse at the
  input; bulk 220–470 µF at entry plus 10 µF near each CPLD/FIFO cluster on
  top of the per-chip 100 nF; consider P-FET reverse-polarity protection
  (a series Schottky's 0.3–0.4 V is unaffordable against the 4.74 V trip).
- Rail droop feeds the §3 bring-up gate (≥4.85 V at the DS1233D-5).
- Thermal: 4× ATF1508AS at ~0.5–0.9 W each are warm but fine in PLCC84 —
  don't box them in tightly; sockets already space them off the board.

## 9. Procurement / bin check

Not in chip-inventory.csv and not confirmed on hand:

- [x] DS1233D-5 on hand (see §3 for the no-pushbutton-detect caveat); DS1233D-10 is the fallback if the rail gate fails
- [ ] 5.5×2.1 mm barrel jack (PCB mount) — adapters on hand
- [ ] SST39SF040 ×2 (DIP-32)
- [ ] ATF1504AS PLCC44 (TIMING) — confirm; only ATF1508s are recorded
- [x] IDT7200L15 ×6 + spare confirmed 2026-08-18: 5 new in the project box
      + 2 on the rev-1 board (the L15 column is the one the §10 table uses)
- [ ] 74HCT155 (user agreed to swap)
- [ ] 74AC541 ×2 (74AC00/74F00 dropped 2026-08-18 — the /RE shaping gate
      was rejected by the pinned timing table, §10 item 1)
- [ ] DS3231 + coin-cell holder
- [ ] 2N7000 ×3 (THT; replaces the 2N7002 callouts)
- [ ] Polyfuses (joystick +5, PS/2 +5, optional 2 A input)
- [ ] LM358 (or salvage rev-1 U15)
- Bin note: the 3× "68681" are MC parts.  **Not** substitutes for the
  XR68C681 — the firmware's 115200 setup uses XR-specific CSR extend
  commands (an MC part comes up at 2400 baud with that init).  Spares for a
  low-baud emergency only.

## 10. Gating items still open before capture (unchanged from griffin.yml, elevated)

1. **FIFO /RE shaping timing table — RESOLVED 2026-08-18** (pinned from
   REN_7200-7202_DST 20171127 and DS20006682A; full table in the griffin.yml
   read-port interfaces entry).  Verdict: the 74AC00 shaping scheme fails
   structurally (data-valid window [gate tPD + tA 15, T/2 + tDV 5] cannot
   reach any capture edge; the rescue needs a ~15 ns delay element whose
   tolerance exceeds its placement window).  Adopted: **registered /RE,
   2 pixel clocks per word, no gate** — worst-case margins +7.7 ns on the
   data and /EF paths, +24.7/+29.7/+54.4 on pulse/recovery/cycle, no
   duty-cycle dependence.  74AC00/74F00 purchase dropped.  Downstream
   (tracked as a griffin.yml issue): compositor.v + vidcmd-spec.md +
   super-engine models assume 1 word/clock; update and refit.
2. **IDT7200 standalone strapping — pinned from the datasheet**: ~XI → GND
   (single-device mode), ~FL/RT → high (retransmit unused; note RT/RS/XI
   inputs spec VIH = 2.6 V — fine, all driven by CPLD rail-swing or tied),
   ~RS pulsed by TIMING at vsync.  Two operational constraints that come
   with ~RS: (a) a reset is REQUIRED after power-up before the first write
   — satisfied once the first vsync has fired, so firmware must not stream
   before then (boot order already does this); (b) both ~RE and ~W must be
   high from tRSS 15 ns before ~RS rises until tRSR 10 ns after — the
   frame-boundary discipline (ENGINE idle at vsync, lists re-armed from
   the ISR) must guarantee no FIFO write overlaps the ~RS pulse.
   Recommend closing the desync-telemetry issue as "frame-bounded /RS
   recovery, no 9th-bit telemetry" — ground D8, NC Q8 (COMPOSITOR is
   pin-closed anyway, so a detector has nowhere to land).
3. **DTACK ownership** (yml issue): resolve as *GLUE owns ~DTACK for every
   region*, totem-pole (as glue.v already implements); the DUART's DTACKN is
   a handshake input to GLUE, never wired to the CPU.  Write this into
   griffin.yml prose.
4. **IPL/IACK prose** (yml issue): GLUE encodes IPL2:0 totem-pole and
   asserts ~VPA on IACK; ~PS2_IRQ is internal-only.  Write into griffin.yml
   so the netlist diff doesn't expect a phantom net.
5. **clocks:/reset: prose section** in griffin.yml (yml issue): DS1233 +
   button + CPU-reset-instruction behavior (§3), two oscillators + DUART
   crystal, GCLK routing rule, Y4 deleted.

## 11. Confirmed non-issues (so they don't get re-litigated)

- CPU bus fan-out (§1) — no transceivers; drop the Board-changes item
  "7200s on the bus may need transceivers"; keep the 74HCT245s as bench
  spares.
- ENGINE ~BR/~BGACK drive: totem-pole in engine.v — no wired-OR hazard
  (single master), pull-ups are blank-CPLD insurance only.
- ~VPA slow-release hazard: none — glue.v drives it totem-pole (a real
  hazard had it been open-drain with rev 1's 100 K).
- RAM/ROM /OE ← nR_W contention: write cycles disable outputs before the
  CPU drives data (~70 ns gap); read-to-write turnaround fine.
- 74AC541/74AC00 input thresholds (3.5 V): all inputs come from
  rail-swing CPLD/oscillator outputs by construction.
- HSYNC/VSYNC fan-out from TIMING: 2 loads each (VGA buffer + PORTS/GLUE
  taps) — trivial.
- DUART IACKN tied high + GLUE VPA autovector: consistent; no vectored
  IACK path exists anywhere.
