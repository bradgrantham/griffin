# PORTS peripheral (new CPLD) — rev 2 fit experiment

Standalone fit experiment in `cpld/ports/` (audio.v style: no griffin.yml,
firmware or emulator wiring, no `//PIN:` lines, `-preassign ignore`).  One
source, `cpld/ports/ports.v`, gated by `` `ifdef ``s; each
`cpld/ports/ports_<config>.v` wrapper defines a feature set and includes it,
so every configuration leaves its own `.fit`.  Run with `make -k ports` from
`cpld/` (not part of `all`).

Question asked: **what combination of PS/2 keyboard + PS/2 mouse + joystick +
paddle (+ stretch: audio) fits an ATF1508AS, and which subset fits an
ATF1504AS?**

## Headline conclusions

1. **Two PS/2 channels do not fit an ATF1508AS — not even by themselves.**
   `ports_ps2_pair` (keyboard + mouse, nothing else) synthesises to 100 FF and
   the fitter needs **140 nodes+FB out of 128 (109 %)**; it fails every pass
   in PLCC84 *and* TQFP100, with `Foldback_logic = on`, with
   `xor_synthesis = on`, and with `Logic_Doubling = on`.  Adding joystick
   (115 %) or joystick+paddle (143 %) only makes it worse.  A second PS/2 port
   still needs its own silicon; PORTS does not rescue it.
2. **One PS/2 channel + both joysticks + both paddles fits an ATF1508AS
   comfortably**: 100/128 logic cells (78 %), 71 FF, 39/64 I/O, 304 product
   terms, 12 foldback, fits on the first pass with no recovery strategy.  This
   is the configuration to build.
3. **The ATF1504AS is viable only in PLCC68, and only for a subset.**  Every
   1504 configuration overflows PLCC44 on **I/O count** (34–36 I/O vs 32), not
   macrocells.  In PLCC68: joystick+paddle fits with room (42/64 LC, 21 FF),
   keyboard+joystick fits with *zero* spare (64/64 LC, 79/64 nodes+FB), and
   keyboard+joystick+paddle is a hard fail (71 FF > 64 macrocells).
4. **The slow-debounce variant is not worth it.**  It saves exactly the
   predicted 4 FF (121 → 117 on the full config, 50 → 48 on keyboard-only), but
   the saved registers buy nothing: the sample enable turns into extra product
   terms on the shift register, and on the 1504 PLCC68 the slow variant turns a
   *fitting* keyboard+joystick (64/64 LC) into a *failing* one (63/64 LC,
   grouping fail).  On the 1508 it is a wash (67 vs 70 LC).  Do not spend a
   GLUE tick on it.
5. **Audio does not belong in PORTS.**  `ports_full_audio` is 151 flip-flops —
   the 1508 fitter rejects it before placement ("Design has 151 flip flop,
   latch or tristate").  Keep audio on its own ATF1504 (see
   pcbv2-audio-design.md, 49/64 LC in PLCC44).

## Results

All runs: `-preassign ignore -strategy JTAG = on`, fitter 1.8.7.8, yosys via
`run_yosys.sh`.  "FF (yosys)" is the DFF count out of synthesis, i.e. the true
register cost; the LC/FF/PT columns come from the `.fit` resource summary.
**Note:** when the fitter fails, its summary block is the wreckage of the last
failed pass (it reports 0 FF and 0 Pts) — for failures the meaningful numbers
are *logic cells* and *nodes+FB/MCells*, which is where the overflow shows.

### ATF1508AS, PLCC84 — the plan's 1508 ladder (`make ports`)

| Config | Features | FF (yosys) | LC | FF (fit) | Foldback | Nodes+FB | I/O | PT | Fit? |
|---|---|---|---|---|---|---|---|---|---|
| ports_ps2_pair | kbd + mouse | 100 | 122/128 (95 %) | — | 18 | **140/128 (109 %)** | — | — | **FAIL** |
| ports_ps2_pair_joystick | + joystick | 100 | 119/128 (92 %) | — | 29 | **148/128 (115 %)** | — | — | **FAIL** |
| ports_full | + paddle | 121 | 127/128 (99 %) | — | 57 | **184/128 (143 %)** | — | — | **FAIL** |
| ports_full_slow_debounce | + slow debounce | 117 | (no summary) | — | — | — | — | — | **FAIL** (fitter `INTERNAL ERROR` after 4 failed passes) |
| ports_full_audio | full + audio | 151 | — | — | — | — | — | — | **FAIL** (rejected: "Design has 151 flip flop, latch or tristate") |

Every failing config that reaches placement reports `uses 90% of the logic
resources` and then dies in placement/grouping/routing.  `ports_full_slow_debounce` reproducibly gets
`Design is large` → `Placement fail` → `Grouping fail` → `Grouping fail` and
then crashes the fitter with `INTERNAL ERROR` before writing a resource
summary (same class of crash as the known VIDEO one); `xor_synthesis = on`
crashed it the same way on `ports_ps2_pair_joystick`.

Recovery knobs from the plan were tried on the failing 1508 configs that do
produce a summary and changed **nothing**: `-strategy Foldback_logic = on` and
`-strategy xor_synthesis = on` reproduce byte-identical LC / foldback /
nodes+FB numbers on `ports_ps2_pair`, `ports_ps2_pair_joystick` and
`ports_full`, and `-strategy Logic_Doubling = on` (which pass 2 tries anyway)
also fails.  `ports_ps2_pair` in **TQFP100** (80 I/O, same 128 macrocells)
fails identically — confirming the wall is logic, not pins.  Knobs are moot
for `ports_full_audio`, which is rejected before placement.

Why 90 % utilisation is already fatal: in the *fitting* `kbd+joy+paddle` build
at 78 % overall, LABs F and H are already 16/16 logic cells and G is 15/16.
The ATF1508's 16-macrocell / 80-PT / 36-fanin LAB granularity means the last
~15 % of the part is effectively unreachable for a design with this much
shared control fan-out.

### ATF1508AS, PLCC84 — configurations that do fit

These are the 1504 wrappers re-fit on the 1508 by hand (same command with
`-d ATF1508AS -p PLCC84`), because the ladder above showed the 1508 is the
right device but two PS/2 channels are not:

| Config | Features | FF (yosys) | LC | FF (fit) | Foldback | Nodes+FB | I/O | PT | Fit? |
|---|---|---|---|---|---|---|---|---|---|
| ports_keyboard_joystick | kbd + joy | 50 | 70/128 (54 %) | 50/128 | 11 | 75/128 (58 %) | 37/64 | 232 | **fits** |
| ports_keyboard_joystick_slow_debounce | + slow debounce | 48 | 67/128 (52 %) | 48/128 | 12 | 76/128 (59 %) | 38/64 | 226 | **fits** |
| **ports_keyboard_joystick_paddle** | **kbd + joy + paddle** | **71** | **100/128 (78 %)** | **71/128** | **12** | **102/128 (79 %)** | **39/64** | **304** | **fits** |

### ATF1504AS — the plan's 1504 set

PLCC44 is the target package in the plan; every configuration fails it on I/O
before the fitter even places logic, so each was re-run in PLCC68 (48 I/O).

| Config | Features | FF (yosys) | PLCC44 | PLCC68: LC | FF | Foldback | Nodes+FB | I/O | PT | Fit? |
|---|---|---|---|---|---|---|---|---|---|---|
| ports_joystick_paddle | joy + paddle | 21 | FAIL, 34 I/O | 42/64 (65 %) | 21/64 | 3 | 35/64 (54 %) | 37/48 | 160 | **fits (PLCC68)** |
| ports_keyboard_joystick | kbd + joy | 50 | FAIL, 34 I/O | 64/64 (100 %) | 50/64 | 15 | 79/64 (123 %) | 37/48 | 178 | **fits (PLCC68), zero spare** |
| ports_keyboard_joystick_slow_debounce | + slow debounce | 48 | fitter crash | 63/64 (98 %) | — | 15 | 78/64 (121 %) | 36/48 | — | **FAIL** |
| ports_keyboard_joystick_paddle | kbd + joy + paddle | 71 | FAIL, 36 I/O | — | — | — | — | — | — | **FAIL** (71 FF > 64 MC) |

(`Nodes+FB/MCells` can exceed 100 % and still fit — foldback nodes are
expander product terms, counted against the same pool but not one-for-one
with macrocells.)

### Cross-checks against the plan's estimates

| Block | Estimated FF | Measured FF (yosys) |
|---|---|---|
| Ps2Channel, fast debounce | 51 | 50 |
| Ps2Channel, slow debounce | 49 | 48 |
| Paddles (2×8 counters + 2×2 sync + dump) | 21 | 21 |
| Joystick | 0 FF, +14 I/O | 0 FF, +14 I/O (`ports_ps2_pair` 100 FF → `ports_ps2_pair_joystick` 100 FF) |
| Audio (divider + HF sync + IRQ latch) | ~30 | 30 (121 → 151) |

The FF estimates were right to within one per channel.  What the estimate
missed is that a PS/2 channel costs far more than its flip-flops on an
ATF15xx: the second channel pushes the design past the node budget through
foldback expansion, not through registers.

## Register map (as implemented in ports.v)

Byte peripheral on the LDS lane; registers on odd addresses, word slot =
A[4:1], A[4] selects the PS/2 channel so the mouse map is the keyboard map
+ 0x10.

| Offset | Read | Write |
|---|---|---|
| 0x01 / 0x03 | — | PS2_KEYBOARD_TX_DATA — A[1] carries firmware's odd parity; the write arms TX |
| 0x05 | PS2_KEYBOARD_STATUS | PS2_KEYBOARD_CLEAR (W1C: bit0 RX_READY, bit1 TX_DONE) |
| 0x07 | — | PS2_KEYBOARD_CTRL (bit0 drive CLK low, bit1 drive DATA low) |
| 0x09 | PS2_KEYBOARD_RX_DATA | — |
| 0x0B / 0x0D | JOYSTICK_PORT_1 / JOYSTICK_PORT_2 | — |
| 0x0F | — | PADDLE_CONTROL (bit0 DUMP: drives PADDLE_DUMP, holds both counters at 0) |
| 0x11 / 0x13 | — | PS2_MOUSE_TX_DATA |
| 0x15 | PS2_MOUSE_STATUS | PS2_MOUSE_CLEAR |
| 0x17 | — | PS2_MOUSE_CTRL |
| 0x19 | PS2_MOUSE_RX_DATA | — |
| 0x1B / 0x1D | PADDLE_A_COUNT / PADDLE_B_COUNT | — |
| 0x21–0x27 | [PORTS_AUDIO only] DIVLO / DIVHI / CTRL+STATUS / CLRINT, selected by A5 | |

STATUS byte is bit-identical to today's GLUE PS2_STATUS:
bit0 RX_READY, bit1 TX_DONE, bit2 TX_ACK, bit3 RX_PARITY, bit4 RX_FRAME_ERR,
bit5 DATA_LIVE, bit6 CLK_LIVE.

JOYSTICK_PORT_x byte follows griffin.yml JOYSTICK.STATE (active low):
bit0 UP, bit1 DOWN, bit2 LEFT, bit3 RIGHT, bit4 FIRE, bit5 PIN9, bit6 PIN5,
bit7 reads 1.

Paddle counters are 8-bit **saturating** upcounters (all-ones terminal, per
the ATF15xx constant-cost guidance) enabled by `PORTS_TICK & ~sense`, so a
pot that never trips reads 0xFF instead of wrapping.

## Signals to route (recommended build: ATF1508AS PLCC84, kbd + joy + paddle)

39 I/O + 1 dedicated input, fitter-placed — final pinout TBD when the `.pin`
is locked.

- Dedicated input: **SYSCLK**
- Inputs from GLUE / bus: **nRESET**, **nPORTS_SELECT** (cycle-qualified,
  reads *and* writes), **A4..A1**, **nLDS**, **R_nW**, **PORTS_TICK**
- Bidirectional: **D7..D0** (PORTS' own byte registers only)
- Open-drain bidirectional: **PS2_KEYBOARD_CLK**, **PS2_KEYBOARD_DATA**
  (external pull-ups, as today on GLUE pins 39/40)
- Joystick inputs (14): **JOYSTICK_1_{UP,DOWN,LEFT,RIGHT,FIRE,PIN9,PIN5}**,
  **JOYSTICK_2_{...}** straight off the DE-9s.  With paddles fitted,
  JOYSTICK_1_PIN9 / JOYSTICK_1_PIN5 double as the paddle A / B comparator
  sense inputs — no extra pins.
- Outputs: **nPORTS_IRQ** (push-pull, active low, into GLUE's IPL encoder),
  **PADDLE_DUMP** (gate of the discharge FET)
- No nDTACK and no nUDS: GLUE keeps threshold DTACK for the region, as it does
  for CF.

If the mouse channel is added later on separate silicon it needs the same
bus fan-out (nPORTS_SELECT or its own select, A[4:1], nLDS, R_nW, D[7:0]).

## Required GLUE changes

- **Delete the PS/2 frame engine** (glue.v:256-405) and its registers.  That
  returns ~51 flip-flops and frees GLUE pins **39/40** (PS2_CLK/PS2_DATA) for
  the new PORTS control lines.
- **Add `nPORTS_SELECT`**: a cycle-qualified region select asserted on reads
  and writes, exactly like `nDUART_SELECT` / `nENGINE_SELECT`.  The freed
  0xFC0000 region (the rev-1 74HC373 audio latch slot) is the natural home.
- **Add `nPORTS_IRQ` as an input** into the priority encoder — it takes over
  the level-4 slot the internal `ps2_irq_active` occupies today.
- **Add a threshold DTACK term** for the PORTS region (0 wait states, same as
  GLUE/ENGINE); PORTS generates no DTACK of its own.
- **Add a free-running prescaler** with a 1-SYSCLK-wide output pulse at
  SYSCLK/512 (`PORTS_TICK`, ≈27.96 kHz at 14.318 MHz → full-scale 8-bit paddle
  ramp ≈ 9.2 ms, inside one frame).  A 9-bit upcounter plus the all-ones
  terminal; GLUE has the room once PS/2 leaves.  The optional ÷8 tap for
  `PORTS_SAMPLE_TICK` is **not** recommended — see conclusion 4.

## Board parts retired

- **2× 74HCT245** — the joystick read transceivers (0xC40000 slot); PORTS
  reads both DE-9s directly.
- **2× 74HC590** — the paddle position counters (0xC80000 slot); PORTS counts
  in the CPLD.
- **DUART OP4/OP5** (`DUART_OP_PADDLE_DUMP` / `~PADDLE_CLR`) and the two-pin
  dump/clear dance in the vsync ISR — replaced by PADDLE_CONTROL bit 0, which
  drives the FET *and* holds the counters at zero in one write.
- One quadrant of the 74155 direct-bus decode (the joystick read strobe and
  the paddle read strobe) becomes free.
- GLUE's PS/2 pins and engine (see above).

Audio is untouched: it keeps its own ATF1504 per pcbv2-audio-design.md.

## Follow-ups (not done here)

- glue.v changes above; a griffin.yml PORTS block (proposed region 0xFC0000)
  with codegen'd registers; firmware `GLUE_PS2_*` → `PORTS_PS2_KEYBOARD_*`
  rename; emulator model.
- Decide the mouse: a second PS/2 channel needs its own device (a second
  ATF1504 in PLCC68 running `ports_keyboard_joystick`-class logic, or the
  earlier "put it in ENGINE" idea), because it demonstrably cannot share a
  1508 with the first one.
- Lock a `.pin` file and re-fit with `-preassign keep` before committing to a
  PCB; all numbers here are with the fitter free to place pins.

---

# Confirming fits (revised architecture)

After the ladder above, the architecture chosen is: **the PS/2 keyboard stays
in GLUE** (it fits there today), and **PORTS = PS/2 mouse + joysticks +
paddles + line-strobe-derived audio FIFO pop**.  PORTS needs no GLUE prescaler
at all: VIDEO's HSYNC is tapped as `LINE_STROBE`, the paddle counters run at
the full line rate (31.469 kHz — full scale 255 x 31.8 us ~= 8.1 ms, half a
frame, matching the 74HC590 RC sizing) and a single toggle FF halves it to
~15.73 kHz for the FIFO pop (`AUDIO_SAMPLES_PER_SECOND`).  No divider
register, no 12-bit counter.

Two confirming fits were run.

## Fit 1 — PORTS: mouse + joystick + paddle + line-strobe audio pop

`cpld/ports/ports_mouse_joystick_paddle_audio.v` (new gates `PORTS_LINE_STROBE`
and `PORTS_AUDIO_POP`), ATF1508AS PLCC84, `-preassign ignore -strategy JTAG = on`.

| Metric | Value |
|---|---|
| FF (yosys) | **80** (estimate was ~80) |
| Logic cells | **111/128 (86 %)** (estimate was ~110) |
| Flip-flops | 80/128 (62 %) |
| Foldback | 16/128 (12 %) |
| Nodes+FB/MCells | 116/128 (90 %) |
| I/O pins | 41/64 (64 %), plus 1/4 dedicated inputs |
| Product terms | 311 (cascades 11) |
| Result | **`$Device PLCC84 fits`** — first attempt, no recovery strategy needed |

**Conclusion: the revised PORTS fits an ATF1508AS PLCC84 with 17 spare logic
cells and 23 spare I/O.**  No decomposition run was needed.

New register: **0x1F**, write `AUDIO_CONTROL` (bit0 enable pops, bit1 W1C the
HF IRQ), read `AUDIO_STATUS` (bit0 IRQ latched, bit1 live half-full-or-more,
bit2 enable).  `LINE_STROBE` gets a 2-FF synchronizer plus a delayed copy
(async, pixel-clock domain); the falling edge of active-low HSYNC is counted.
`nFIFO_HF` gets the same treatment, and its rising edge (FIFO below half full)
latches the IRQ into `nPORTS_IRQ`.  `PORTS_TICK` is no longer declared in this
configuration — GLUE needs no tick generator.

The nine original wrappers were re-elaborated after the ports.v change and
produce byte-identical flip-flop counts (100 / 100 / 121 / 117 / 151 / 21 /
50 / 48 / 71), and `ports_keyboard_joystick_paddle` re-fits to exactly the
same PLCC44 result (`# ERROR : Design has 36 IOs.`).

## Fit 2 — GLUE rev-2 delta (keyboard retained + PORTS support)

`cpld/glue/glue_rev2_ports.v` (a copy of glue.v; **glue.v itself untouched**)
with: ROM window moved to 0x800000-0xBFFFFF (`A23 & ~A22`); `AUDIO_LE` and its
decode deleted; new `nIO_RD_EN` / `nIO_WR_EN` direct-bus strobes for
0xC00000-0xCFFFFF (write qualified on UDS & LDS per griffin.yml); new
`nPORTS_SELECT` on the freed pin 68; new `nPORTS_IRQ` input at autovector
level 2; a 0-wait-state PORTS DTACK term.  Everything else byte-identical —
the PS/2 keyboard engine stays.

56 flip-flops out of yosys, same as production GLUE.

| Run | Flags | Result |
|---|---|---|
| **control**: production glue.v | `-preassign keep … debug = on` | **fits** — 117/128 LC (91 %), 56 FF, 16 foldback, 113/128 nodes+FB (88 %), 59/64 I/O + 2/4 dedicated, 347 PT |
| glue_rev2_ports | same as production (`-preassign keep … debug = on`) | **FAIL** — `Fail to route variable A_hi_2 in Block 2`, `A_lo_0 in Block 3`, then placement fail on every later pass; "uses 90 % of the logic resources" |
| glue_rev2_ports | (a) dropped `-strategy debug = on` | **FAIL**, identical failure and identical numbers |
| glue_rev2_ports | (b) added `-strategy Foldback_logic = on` | **FAIL**, identical failure and identical numbers |
| glue_rev2_ports | **diagnostic**: `-preassign ignore` (fitter free to place pins) | **fits** — 107/128 LC (83 %), 56 FF, 12 foldback, 119/128 nodes+FB (92 %), 60/64 I/O + 4/4 dedicated, 306 PT |

**Conclusion: the rev-2 GLUE logic fits an ATF1508AS with room (107/128 LC);
what does not fit is the rev-1 hand pinout plus three new pins.**  The delta
adds `nIO_RD_EN`, `nIO_WR_EN` and `nPORTS_IRQ` (net +3 pins;
`nPORTS_SELECT` reuses AUDIO_LE's pin 68) on top of a pinout where 59 of 64
I/O were already nailed down, and the fitter cannot route the leftovers into
the LABs the frozen assignments imply.

Since rev 2 is a board respin, the pinout is re-assignable, so the fix is a
**new `.pin`, not less logic**: take the placement from the `-preassign ignore`
run and constrain the rev-2 layout to it.  Note it lands nearly pin-full —
60/64 I/O and all 4 dedicated inputs — so GLUE has essentially no spare pins
left after this delta.  Anything further wants either a bigger package
(TQFP100, 80 I/O) or a function moved to PORTS.

No logic was shaved.

## Register map: griffin.yml is authoritative

The PORTS register map adopted into griffin.yml on 2026-07-28 **supersedes the
offsets used by the experiment source in `cpld/ports/ports.v`**.  The adopted
map puts the mouse channel at the same offsets GLUE uses for the keyboard —
TX_DATA 0x09 (parity in address bit 1, alias 0x0B), STATUS/CLEAR 0x11, CTRL
0x13, RX_DATA 0x15 — so firmware's ps2.cpp can become one base-parameterized
driver serving both channels, with joysticks at 0x01/0x03, paddle counts at
0x05/0x07, PADDLE_CONTROL at 0x0F and AUDIO_CONTROL/AUDIO_STATUS at 0x1F.

The experiment's internal offsets were chosen for the fit ladder (both PS/2
channels present, mouse = keyboard + 0x10) and were never meant to be the
contract.  `ports.v` gets realigned to the griffin.yml map at integration
time; the decode is a handful of localparams and does not change the measured
utilisation.
