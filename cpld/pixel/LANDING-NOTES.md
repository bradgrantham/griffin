# Indexed PIXEL + TIMING merge — landing notes, package 1

Scope of package 1: **RTL and chip testbenches only.**  `super-engine/render.*`,
the suite, the emulator, firmware, `griffin.yml` and `vidcmd-spec.md` are later
packages; everything they need is written down here.  Nothing outside
`cpld/pixel/`, `cpld/Makefile` and this file was touched, and nothing was
committed.

The contract is `indexed-pixel-spec-draft.md`; the evidence behind the design is
`cpld/experiments/indexed-pixel/RESULTS.md` (left untouched as the record).
The pre-change state is tagged `rev1-video`.

**Status: A, B, C, D, E all landed and passing.**  What is deliberately not
done is at the end.

---

## 1. Fit record — ATF1508AS PLCC84, production strategy flags

Flags on every row, the production `pixel:` target verbatim:
`-preassign keep -strategy debug = on -strategy TDI_pullup = on
-strategy TMS_pullup = on -strategy power_reset on -strategy JTAG = on`, tpd 15.

| rung | pins | fits | pass | LC /128 | FF /128 | Nodes+FB /128 | foldback | PT | I/O /64 | ded. in |
|---|---|---|---|---|---|---|---|---|---|---|
| experiment `r8w` (reference) | standalone-PIXEL block | yes | 1 | 122 | 95 | 130 (101%) | 26 | 401 | 43 | 2 |
| A1 merged, OLD production pins kept | explicit | yes | 1 | 116 | 95 | 141 (110%) | 36 | 372 | 43 | 2 |
| A2 merged, FREE placement | none (empty .pin) | yes | 1 | 116 | 95 | 136 (106%) | 31 | 367 | 43 | 2 |
| **A3 = A2 harvested into //PIN, re-fit** | explicit | **yes** | **1** | **116** | **95** | **140 (109%)** | 35 | **371** | **43** | **2** |

A3 is what `make pixel` builds today.  No levers were needed — no
`xor_synthesis`, no restructuring, no ladder beyond these three rows — so none
were spent.

* **A1 vs A2.**  The owner released the old pin numbers mid-task (rev-2 is not
  routed).  Keeping them costs nothing in logic cells and 5 nodes+FB / 5 PT of
  routing pressure.  Both fit on pass 1, so either would have shipped.
* **A2 was a deliberate free placement.**  `pixel.v` carried no //PIN block, so
  `run_fitter.sh` wrote an empty `.pin` and `-preassign keep` placed every
  signal itself.  That silent-free-placement behaviour is the known 2026-09-04
  trap; here it was the intent, and it is called out in `pixel.v`'s pin block.
* **A2 vs A3.**  The harvested block reproduces the free fit's **placement
  exactly** (a diff of the named pins is empty) and the same LC / FF / I/O, but
  nodes+FB and PT come out 140/371 against 136/367: the fitter's choice of
  internal node macrocells is not bit-identical between a free run and a keep
  run.  "Re-verified identical" would be an overstatement — "same placement,
  same logic cells, four more foldback nodes" is the truth.
* **Why this beats the experiment's 122 LC.**  Same RTL.  `r8w` was fitted with
  the standalone-PIXEL //PIN block in force; A2/A3 let the fitter place all 41
  signals.  Six logic cells and thirty product terms are what that constraint
  was charging.
* **Nodes+FB reads over 100% and the design fits.**  The fitter does not
  enforce that line as a cap — production `compositor.v` reads 143/128 (112%)
  today.  The resource with real headroom here is **twelve logic cells**
  (116/128) and 33 flip-flops.

Per-LAB fill, A3 (macrocells / I/O / foldback / PT, cascades in brackets):

| LAB | MC | I/O | FB | PT | casc |
|---|---|---|---|---|---|
| A | 14/16 | 8/16 | 7/16 | 52/80 | 2 |
| B | 9/16 | 8/16 | 4/16 | **71/80** | 7 |
| C | 12/16 | 8/16 | 3/16 | 41/80 | 2 |
| D | 11/16 | 8/16 | 2/16 | 47/80 | 0 |
| E | 14/16 | 2/16 | 2/16 | 50/80 | 0 |
| F | **16/16** | 1/16 | 7/16 | 45/80 | 0 |
| G | 13/16 | 4/16 | 6/16 | 32/80 | 0 |
| H | **16/16** | 4/16 | 4/16 | 33/80 | 0 |

So the twelve spare cells are **not evenly reachable**: F and H are full and B
is the product-term hotspot.  Anything much bigger than a couple of flip-flops
should be expected to move pins or fail.

---

## 2. Pin map delta — for the rev-2 board and for `griffin.yml`

`cpld/pixel/pixel.v`'s //PIN block is the one home for the numbers, and it is
marked **PROPOSED rev-2, NOT FROZEN**: harvested from the free fit, confirmed by
a keep re-fit, to be frozen when layout starts.

* **The TIMING chip disappears.**  Its ATF1504AS PLCC44 socket, its JTAG
  position and the three strobe traces (PIX_CONSUME, PIX_PRELOAD, PIX_LAST) all
  go.  JTAG chain becomes GLUE -> PORTS -> ENGINE -> PIXEL -> COMPOSITOR.
  `timing.v` keeps the Timing module and its laws, has no //PIN block any more,
  and is `include`d by `pixel.v`; `make timing` is retired.
* **TIMING's eight external outputs move onto PIXEL**: VGA_HSYNC 4, VGA_VSYNC
  6, nRS 5, H_ACTIVE 79, HBLANK 69, nVSYNC_IRQ 80, PADDLE_TICK 81, AUDIO_TICK
  70.  Consumers unchanged: COMPOSITOR (H_ACTIVE, nRS — and the FIFOs' /RS),
  ENGINE (HBLANK), GLUE (nVSYNC_IRQ), PORTS (the two ticks), VGA connector.
* **PIXELS FIFO becomes a ganged 16-bit port**: eight more traces.
  `PIXELS_Q[15:8]` is the HIGH-byte 7200 (the old EVEN part, D[15:8]) on pins
  73, 11, 9, 22, 21, 18, 17, 44; `PIXELS_Q[7:0]` the LOW-byte part (old ODD,
  D[7:0]) on 10, 35, 37, 36, 33, 46, 12, 63.  The shared-Q-bus arrangement and
  the second strobe are retired: ONE `nPIXELS_RE` (pin 15) drives both parts.
* **PIXEL's RGB_OUT[11:0] bus is gone.**  In its place `IDX[3:0]` (24, 27, 28,
  29) and `nCLUT_WE` (30) to three 16x4 separate-I/O palette RAMs (74F219
  pinout, /CS tied low), whose **data inputs come from COMPOSITOR's
  `set_pix_value[11:0]`** and whose outputs drive COMPOSITOR's `RGB_IN[11:0]`.
* **`set_pix_value` to PIXEL narrows from 12 bits to 4**: PIXEL taps only
  `[3:0]` (pins 8, 40, 39, 34).  COMPOSITOR still exports all twelve, now to the
  RAMs as well.
* **Dedicated inputs unchanged**: PIXEL_CLK on GCLK1 (83), nRESET on GCLR (1).
  Pins 2 and 84 stay free.  JTAG on 14/23/62/71 as the fitter reserved them.
* **Reserved, not fitted**: DUART OP4..OP7 -> 54, 55, 56, 57 (the future TIMING
  mode/rate selector; the reservation used to live on timing.v's PLCC44 pins
  20/21/24/25).
* COMPOSITOR is **unmodified** — RTL, pins and VIDCMD word formats unchanged.
* **A board note that only the pair bench exposed**: COMPOSITOR's only reset
  input is TIMING's /RS, which pulses once per frame at vsync, so from power-up
  until the first vsync the chip is held sane only by the ATF1508's own
  POWER_RESET feature (the `power_reset on` strategy flag).  That flag is
  therefore load-bearing for COMPOSITOR, not cosmetic.  The pair bench has to
  model it explicitly or the whole first frame runs with X state.

---

## 3. Measured constants (what packages 2+ must match)

All of these are asserted by a bench, and every one was hand-derived from the
documented law before simulating.

**Read cadence** — W stream bits per group, R clocks per group, so a 16-bit word
lasts 16R/W consumption clocks:

| mode | `mode[1:0]` | T full | T half | clocks/word full | half | words/line full | half |
|---|---|---|---|---|---|---|---|
| 1bpp | 00 | 14 | 15 | 16 | 32 | 40 | 20 |
| 2bpp | 01 | 12 | 14 | 8 | 16 | 80 | 40 |
| 4bpp | 10 | 8 | 12 | 4 | 8 | UNDEFINED | 80 |
| attr | 11 | 6 | 7 | 8 | 16 | 80 | 40 |

(attr's trigger compares `bit_pos[2:0]` only — its word carries eight pattern
bits, so its cell is eight groups.)

**/RE contract.**  Registered output, low for exactly ONE clock per word, never
low on two consecutive clocks.  In-line gap = the word period; preload-to-first
gap = `t1 + 16` where `t1` is the first trigger's clock; exact law
`min gap = (reads > 2) ? min(period, t1+16) : t1+16`; **floor over every defined
mode and skip = 8 clocks** (318 ns at 25.175 MHz, against COMPOSITOR's qualified
2 clocks on the same part).

**Words per line, with the record+1 rule.**

    words per line = record + (pixel_skip != 0 ? 1 : 0)
    record = 40 (1bpp full)  20 (1bpp half)  80 (2bpp full)  40 (2bpp half)
             80 (4bpp half)  80 (attr full)  40 (attr half)

Asserted at the bench's 64-clock width and **on the DUT at the real 640** for
all 65 mode/skip combinations, two consecutive lines each.  Attribute mode takes
no skip, so it is exactly its record.  A skipped line's TAIL SHOWS REAL DATA,
not zeros — the one visible semantic change from the byte engine.

**Skip law.**  Four-bit register counting stream bits; pixel 0 is stream bit
`s` of the line's first word; no whole-word discard.  Depth clamp: 1bpp all four
bits, 2bpp `s & ~1`, 4bpp `s & ~3`, attr forced to 0.  Range clamps: `s <= 16-2W`
at FULL rate only (trigger lead) and `s <= 14` at BOTH rates (preload lead), so
the effective caps are 14 (1bpp either rate), 12 (2bpp full), 14 (2bpp half), 12
(4bpp half).  Clamped **at consumption**, i.e. at the line's preload, so SET
ordering cannot smuggle a stale bit in.  The one-word allowance is **latched at
the preload** (`line_skipped`), so a mid-line `SET(pixel_skip)` affects the next
line only — including its word count.

**Output latency.**  `grp` registers the group consumed at C_k on the edge that
ends C_k and IDX registers one edge later, so **IDX carries pixel k during
C_k+2: IDX lags PIX_CONSUME by 2 clocks and H_ACTIVE by 1.**  Measured directly
(case R2).  This is the retired `ham_held -> RGB_OUT` depth exactly, so
`PIXEL_OUT_LEAD` is still 1, `SPLIT_DAC_LEAD` is still 4, and **no TIMING
constant moves**.  The palette RAM is combinational and costs no clock.

**Reset (/RS at vsync).**  `pix_mode`, `pix_base`, `pixel_skip`, `clut_index`
all 0; IDX 0; nCLUT_WE high; nPIXELS_RE high.  So an untouched chip is 1bpp FULL
rate out of palette entries 0 and 1 with no SET of any kind.  **The palette RAM
is never reset**, powers up undefined, and its contents persist across frames —
measured: an entry written before /RS is still there after it, and an unloaded
entry reaches the DAC as X.

---

## 4. THE PALETTE WRITE — the pinned rule (spec draft section 4)

Clocks are counted from the clock in which `set_pix_commit` is high for the
`clut_data` SET (clock 0).  Measured, clock by clock, by `clut_pair_tb.v`:

| clock | IDX | nCLUT_WE | set_pix_value | RAM outputs (RGB_IN) |
|---|---|---|---|---|
| 0 | pixel index | high | V (the clut_data SET's own payload) | the pixel's colour |
| 1 | pixel index | high | V | the pixel's colour |
| 2 | **clut_index** | high | V (**from the filler**) | the ENTRY being written — wrong, but defined |
| 3 | clut_index | **LOW** | V (from the filler) | **undefined** (high-Z) |
| 4 | clut_index | high | the next word's payload | **undefined** (write recovery) |
| 5 | pixel index | high | next word | the pixel's colour again |

**HOLD — and the filler is NECESSARY, not decoration.**  COMPOSITOR registers
`set_pix_value` from every word it stages, and both its blanking cadence and its
active-video cadence stage one every TWO clocks, so the `clut_data` SET's own
payload owns the bus for clocks 0 and 1 ONLY.  **The data present while /WE is
low belongs to the NEXT WORD IN THE STREAM.**  Measured hold, in clocks from
clock 0, with the third word varied:

| third word | hold | the write stores |
|---|---|---|
| (a) another PIXEL-target SET, different value | **2** | that word's payload — WRONG |
| (b) a COMPOSITOR-target SET (target 0/1), different value | **2** | that word's payload — WRONG |
| (c) **same-value `SET target 7`** | **4** (clocks 0..3) | V — correct |
| (d) nothing (FIFO empty) | indefinite | V — correct |

So a table entry is THREE WORDS and the third must be `SET target 7` carrying
**the same twelve bits**.  It must be target 7 because every other target either
writes a PIXEL register with the colour payload (2, 4, 5, 6), writes a
COMPOSITOR colour (0, 1) — neither of which protects the bus, since the bus is
written from every staged word regardless of target — or, if it is a second
`clut_data`, starts a second mis-timed write.  Sufficient: yes, in whole-clock
terms (setup and the whole pulse are covered).

**WHERE — the disturbed window.**  IDX is stolen for clocks 2, 3, 4.  RGB_IN is
WRONG at clock 2 (it reads the entry being written) and UNDEFINED at clocks 3
and 4 (pulse, then one clock of write recovery).  COMPOSITOR registers RGB_OUT
one clock later, so the pixels it paints wrong are clocks 3..5.

> **THE RULE A LIST AUTHOR FOLLOWS: pixel clocks 2..5 after the commit must be
> in blanking or covered by a non-passthrough source (a `cmp_color`/RUN_COLOR
> RUN, or an opaque MASK dibit).  In active video that means the word after the
> filler must not be a passthrough RUN — it is consumed at clock 4 and takes
> effect from clock 5 — so put one more record (a second same-value `SET target
> 7` does) between the filler and the RUN that resumes passthrough.**

The measured disturbance is clocks 2..4; the fourth clock in the rule is the
write-recovery guard, because the model's one-clock recovery is an abstraction
of a datasheet parameter that is **not pinned yet** (spec draft section 6) and
the guard costs nothing.

**The one place the three-word entry leans on an unread datasheet number.**
`set_pix_value` changes on the very edge /WE rises (measured: one such event per
entry whenever a differently-valued word follows the filler).  In clock terms
the data was valid for the whole pulse, but the RAM's **data hold time from
write-disable** is then met only by whatever skew there happens to be between
PIXEL's /WE output and COMPOSITOR's data outputs — two different packages.  Two
cheap fixes exist, NEITHER APPLIED (the spec draft fixes the clock numbers and
this package does not change the contract):

1. A **fourth word** — a second same-value `SET target 7` — which buys a full
   clock of data hold and is what the "where" rule above already recommends for
   active video anyway.
2. **Move PIXEL's pulse one clock earlier**: `clut_seq = set_cdata | wr1 | wr2`
   and `nCLUT_WE <= ~wr1`, giving IDX = clut_index on clocks 1, 2, 3 and /WE low
   on clock 2.  The entire pulse then sits inside the two words that already
   carry V, so address AND data are stable for the clock before, the clock of
   and the clock after the pulse; the disturbed window becomes 1..4; and `wr3`
   disappears, so it costs **one flip-flop less**.  The filler is still
   necessary (clock 2's data comes from it).  This changes spec section 4's
   clock numbers and the model, so it belongs to a decision, not to this
   package.

---

## 5. Bench inventory

| bench | make target | what it covers | result |
|---|---|---|---|
| `pixel_tb.v` | `make pixel-sim` | the decoder: modes, cadence, skip, words/line | **PASS**, 148 cases, 322 checks, 266 line-runs, 0 failures |
| `mutate.py` | `make pixel-mutants` | 8 single-mistake mutants of `pixel.v` | all 8 FAIL as required (25 / 72 / 193 / 25 / 265 / 25 / 117 / 3 FAIL lines) |
| `timing_tb.v` | `make timing-sim` | the raster laws, against the MERGED top level | **PASS**, 14 checks, 0 failures; all 10 `-DMUTATE=n` controls fail |
| `clut_pair_tb.v` | `make clut-pair-sim` | COMPOSITOR + PIXEL + three RAM models | **PASS**, 11 cases, 57 checks, 0 failures |

`pixel_tb.v` is built from the experiment's `idxw_tb.v`; the retired cases
(micro-HAM, the 2bpp fixed four colours, the shared-bus byte reads, the
extra-byte discard) are DELETED, not kept dormant.  New since the experiment:
the reset-value case, the measured output latency, and the whole
words-per-line sweep re-run on the DUT at the real 640-clock line width.

`clut_pair_tb.v`'s cases: S0 unloaded palette reaches the DAC; S1a/S1b the two
illegal third words (both DETECTED); S1c the contract; S1d starvation; S2
back-to-back entries; S3 eight entries in one hblank then the line that uses
them; S6 a write behind a `cmp_color1` RUN mid-line (undisturbed) and S6-neg the
same write under passthrough (DETECTED, 3 clocks of X reaching RGB_OUT); S7 the
disturbed window logged clock by clock; S8 a sixteen-entry load in vertical
blank after the chip's own /RS, then one line each of 1bpp, 2bpp, 4bpp-half and
attribute checked pixel-exact on RGB_OUT.

Two harness bugs were found and fixed, and both are worth knowing because they
would have produced a confident wrong answer:

* `@(posedge clk)` resumes BEFORE the DUT's non-blocking updates, so the value
  read at iteration *i* is the one held during cycle C0+*i*.  Written as *i*+1,
  the latency check reported 3 against a derivation of 2.  The derivation was
  right (every other case's sampler independently confirms it).
* "the data was stable around the write pulse" is NOT "the data was correct".
  An illegal third word leaves the bus perfectly stable at ITS OWN payload.  The
  criterion has to be "the data at the pulse is the payload the `clut_data` SET
  committed", which is what the bench now asserts.

---

## 6. What packages 2+ need

**`super-engine/render.{h,cpp}`** — `PixelUnit` becomes index-only:

* Delete `pal_fg_`, `pal_bg_`, `ham_held_`, `mode_ham()`, `mode_idx2()`,
  `idx_colour()` and the micro-HAM decoder.  `next_pixel()` returns an INDEX,
  not an `Rgb444`; a new palette unit turns it into one.
* Modes are an ENUM in `mode[1:0]` plus the half-rate flag in `mode[2]`, not a
  bitwise field: 00 1bpp, 01 2bpp, 10 4bpp, 11 attribute.  `mode[3]` reserved,
  must be 0.
* Index formation: 1bpp `{pix_base[3:1], bit}`, 2bpp `{pix_base[3:2], dibit}`,
  4bpp the nibble, attribute `bit ? fg : bg` from `{fg[3:0], bg[3:0],
  pattern[7:0]}` with the pattern MSB first.
* The skip law of section 3 — including BOTH range clamps, which the current
  model does not have at all (it implements skip as `refill(skip); take(skip)`
  with no fetch cadence), and the `record + (skip != 0)` word count.  This is
  the divergence `RESULTS.md` part 4 flagged: the model renders the ideal
  sequence where the hardware clamps.
* A palette unit with **sixteen 12-bit entries, RANDOM at power-up, never
  reset**, and an illegal-window write flag: a write whose clocks 2..5 are not
  in blanking and not covered by a non-passthrough source is a modelled ERROR,
  which is exactly what `clut_pair_tb.v` detects in S6-neg.
* Attribute mode ignores `pixel_skip` entirely.

**`super-engine/descriptor.h`** —

* `SET_PIX_PAL_FG` (2) -> `SET_PIX_CLUT_INDEX`, `SET_PIX_PAL_BG` (3) ->
  `SET_PIX_CLUT_DATA`, `SET_PIX_HAM_HELD` (4) -> `SET_PIX_BASE`; 5 and 6 keep
  their meanings; 7 stays spare and becomes the WRITE FILLER.
* `PIXEL_MODE_DIRECT_1BPP/MICRO_HAM/INDEXED_2BPP` -> the four-value depth enum;
  `PIXEL_MODE_HALF_RATE` stays bit 2.
* Words per line: 40/20 (1bpp), 80/40 (2bpp), -/80 (4bpp), 80/40 (attr), plus
  the `+1` for a nonzero skip.  `PIXELS_WORDS_MICROHAM` and
  `PIXELS_WORDS_INDEXED2` go; the 4bpp-full case must be refused, not computed.
* A palette-entry cost constant: **3 VIDCMD words per entry**, 8 entries in a
  26-word hblank packet, 16 entries in vertical blank.
* `PIXEL_FETCH_LEAD` 16, `PIXEL_OUT_LEAD` 1, `SPLIT_DAC_LEAD` 4 — all unchanged.

**Suite cases (`super-engine/main.cpp`)** — these die with the RTL they
describe: `HAM_REGRESSION`, `HAM_HALF_MASKED`, `MODE_RESERVED_11`,
`IDX2_ALL_FOUR`, `HALF_IDX2`, and the `SET_PIX_HAM_HELD` ordering case around
line 1089; `FB_HAM_BASE`/`FB_HAM_STRIDE` and the `.ham` backdrops go with them.
`1BPP_REGRESSION` and `HALF_1BPP` survive in form but must be re-expressed
against a loaded palette.  What replaces them: one authored screen per mode
(1bpp, 2bpp, 4bpp half, attribute at both rates), a per-line palette reload, a
skipped line asserting **record+1** words consumed, and one deliberately illegal
write that the model must flag.

**`griffin.yml`** — SET targets 2/3/4 renamed, the mode field becomes a depth
enum plus the half-rate bit with bit 3 reserved, words-per-line constants
updated, the "Stream FIFO parts" entry rewritten for a ganged 16-bit PIXELS read
with one strobe, a new palette-RAM interface entry (three 16x4 separate-I/O
parts, data from COMPOSITOR's `set_pix_value`, address and /WE from PIXEL, never
reset), the TIMING chip entry folded into PIXEL, and the three strobes deleted
as board nets.  The deliberate exception worth adding: **the write filler**.

**Emulator** — inherits through `render.cpp`; it must rebuild clean and the
palette must be random at power-up there too.

---

## 7. Open issues / not done

* **No timing arithmetic.**  The palette READ path (PIXEL tCO + RAM tAA +
  COMPOSITOR tSU inside one 39.72 ns dot clock — the spec draft's 22.7 ns
  budget) and the WRITE path (/WE pulse width, address and data setup/hold,
  write recovery) are section 6's job and are NOT settled here.  Nor are the
  sixteen PIXELS inputs sampled on one GCLK edge.  The pair bench's one-clock
  write recovery is an abstraction, not a datasheet number.
* **The data-hold-at-/WE-rise question** in section 4 needs the datasheet of the
  part actually bought, and then a decision between the fourth word and the
  earlier pulse.  Until then the rule in section 4 is written to be safe under
  either.
* Per-equation worst PT: the fitter reports only per-LAB totals, so that column
  stays empty, as in the experiment.
* The `clut_index` post-increment idea (2 words per entry instead of 3) is
  untried and uncosted; with twelve logic cells spare it is not obviously
  unaffordable, but F and H are full LABs.
* The pair bench models the PIXELS FIFO as returning the SAME word to every
  read (a stream of identical words), because the per-word sequencing is
  `pixel_tb.v`'s subject.  A pair-level check with a varying PIXELS stream is
  not done.
* `mutants/` is generated by `make pixel-mutants` and is not checked in.
* 4bpp FULL rate remains UNDEFINED and untested by design (two fetch triggers
  inside the eight-clock PIX_LAST window; 160 words/line is over budget).
* Mid-line mode changes remain undefined and unspecified, as the spec draft says.
