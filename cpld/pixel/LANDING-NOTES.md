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
| A3 = A2 harvested into //PIN, re-fit | explicit | yes | 1 | 116 | 95 | 140 (109%) | 35 | 371 | 43 | 2 |
| A4 write pulse moved 1 clock earlier, A3's pins | explicit | **NO** | routing fail on 1, 2, 3 | — | — | — | — | — | — | — |
| A5 same RTL, FREE placement | none (empty .pin) | yes | 1 | 123 | 94 | 123 (96%) | 17 | 388 | 43 | 2 |
| **A6 = A5 re-harvested into //PIN, re-fit** | explicit | **yes** | **1** | **123** | **94** | **119 (92%)** | 13 | **384** | **43** | **2** |

A6 is what `make pixel` builds today.  No levers were needed — no
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
* **Nodes+FB reads over 100% on A1-A3 and the design still fits.**  The fitter
  does not enforce that line as a cap — production `compositor.v` reads 143/128
  (112%) today.
* **A4/A5/A6: moving the palette write pulse one clock earlier.**  It costs
  **+7 LC and +13 PT and saves one flip-flop**, and — the part that was NOT
  predicted — **it does not route against A3's harvested pinout at all**: three
  fitter passes, `Routing fail` on each, `Design does not fit`.  A fresh free
  fit places it on pass 1 at 123/128 LC, and re-harvesting that placement
  reproduces it exactly (diff of the named pins empty) at 123 LC / 94 FF / 119
  nodes+FB / 384 PT.  The cost is `set_cdata` — live, and a function of
  `set_pix_valid`, `set_pix_commit` and the target decode — now feeding the four
  IDX macrocells, where a registered `wr3` used to.  There is no way around that
  for a pulse this early: at clock 0 no register knows a write has started yet.
* **So the pinout is now on its second harvest, and that is the lesson to carry
  into the freeze**: a placement harvested from a near-full chip is a fit of ONE
  netlist, not a contract the next netlist inherits.  `pixel.v`'s pin block says
  so.  **Remaining headroom: five logic cells (123/128), 34 flip-flops.**

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

**Changed 2026-09-20 after the first draft of this package**: the sequence moved
one clock earlier, so that the pulse is centred in the address window and the
data gets a full clock of setup and a full clock of hold.  The old sequence
(IDX 2..4, pulse at 3) is kept only as `mutants/old_pulse.v`, the negative
control for the assertion that removed it.

Clocks are counted from the clock in which `set_pix_commit` is high for the
`clut_data` SET (clock 0).  Measured clock by clock by `clut_pair_tb.v` (S7):

| clock | IDX | nCLUT_WE | set_pix_value | RAM outputs (RGB_IN) |
|---|---|---|---|---|
| 0 | pixel index | high | V (the clut_data SET's own payload) | the pixel's colour |
| 1 | **clut_index** | high | V — **setup** | the ENTRY being written: wrong, but defined |
| 2 | clut_index | **LOW** | V (**from the filler**) | **undefined** (high-Z) |
| 3 | clut_index | high | V (from the filler) — **hold** | **undefined** (write recovery) |
| 4 | pixel index | high | the next word's payload | the pixel's colour again |

**HOLD — the filler is NECESSARY, and this placement is the only one that
works.**  COMPOSITOR registers `set_pix_value` from every word it stages, and
both its blanking and its active-video cadence stage one every TWO clocks, so
the `clut_data` SET's own payload owns the bus for clocks 0 and 1 and the word
after it owns clocks 2 and 3.  Two words of ownership, three clocks of window,
and they line up exactly — there is no slack in either direction.  Measured hold,
in clocks from clock 0, with the third word varied:

| third word | hold | what the write does |
|---|---|---|
| (a) another PIXEL-target SET, different value | **2** | breaks SETUP and stores that word's payload — WRONG |
| (b) a COMPOSITOR-target SET (0/1), different value | **2** | identical to (a) — WRONG |
| (c) **same-value `SET target 7`** | **4** (clocks 0..3) | stores V, full setup and full hold |
| (d) nothing (FIFO empty) | indefinite | stores V |

It must be target 7 because every other target either writes a PIXEL register
with the colour payload (2, 4, 5, 6), writes a COMPOSITOR colour (0, 1) — neither
of which protects the bus, since COMPOSITOR writes it from every staged word
whatever the target — or, as a second `clut_data`, starts a second mis-timed
write.  **Is a two-word entry legal now?  Only in case (d)**, i.e. when the FIFO
happens to be empty behind the data SET, which ENGINE's prefill makes
unpredictable; it is a measured behaviour, not a contract.  Any word that does
arrive owns clocks 2-3, so it must carry the same twelve bits — which is the
filler.  The word BEFORE the entry is unconstrained (clocks 0 and 1 belong to
the data SET itself), so the entry is exactly three words and no more.

**WHERE — the disturbed window.**  IDX is stolen for clocks 1, 2, 3.  RGB_IN is
WRONG at clock 1 (it reads the entry being written) and UNDEFINED at clocks 2
and 3 (pulse, then one clock of write recovery).  COMPOSITOR registers RGB_OUT
one clock later, so the pixels it paints wrong are clocks 2..4.

> **THE RULE A LIST AUTHOR FOLLOWS: pixel clocks 1..4 after the commit must be
> in blanking or covered by a non-passthrough source (a `cmp_color`/RUN_COLOR
> RUN, or an opaque MASK dibit).  Clocks 1..3 are the entry's own slots; clock 4
> is the first clock of the word after the filler, so THAT WORD MUST NOT BE THE
> RUN THAT RESUMES PASSTHROUGH — put one more record between them.  It may
> otherwise be anything at all, including a differently-valued SET: the write is
> over by then.**

The measured disturbance is clocks 1..3; the fourth clock in the rule is the
write-recovery guard, because the model's one-clock recovery is an abstraction
of a datasheet parameter that is **not pinned yet** (spec draft section 6), and
the guard costs nothing.

### Exact wording proposed for spec draft section 4

> ## 4. Palette write
>
> Sequence, in pixel clocks after the `clut_data` commit: clock 1 `IDX` =
> `clut_index`; clock 2 `nCLUT_WE` low; clock 3 `nCLUT_WE` high with address and
> data still stable; clock 4 `IDX` returns to the pixel index.  The one-clock
> pulse is centred in the three-clock address window, so both the address and the
> data are stable for a full clock before it falls and a full clock after it
> rises.  RAM outputs are high-Z while `/WE` is low and need the part's
> write-recovery time afterwards.
>
> Rules for the list author:
>
> 1. **Where.**  A write disturbs the pixel bus for clocks 1..4 after the commit
>    (1..3 measured, plus one clock of guard for write recovery).  Legal only in
>    blanking, or where those pixel slots are covered by a non-passthrough RUN /
>    opaque MASK.  Clocks 1..3 are the entry's own three words; clock 4 is the
>    first clock of the word after the filler, so that word must not be the RUN
>    that resumes passthrough.
> 2. **Hold.**  `set_pix_value` must carry the payload across clocks 1, 2 and 3.
>    COMPOSITOR re-registers the bus from every staged word and stages one every
>    two clocks, so the `clut_data` SET's own value covers clocks 0 and 1 only
>    and the word after it covers 2 and 3: a `clut_data` SET is therefore
>    followed by one filler word carrying the SAME 12-bit value, `SET target 7`.
>    It must be target 7 — any other target either writes a register with the
>    colour payload or starts a second write — and it is necessary, not
>    decorative.  (Pinned by `cpld/pixel/clut_pair_tb.v`, which also carries the
>    negative control for the one-clock-later sequence this replaced: that one
>    gave the RAM zero data hold, met only by inter-chip skew.)
> 3. Cost per entry: `SET clut_index`, `SET clut_data`, filler = 3 words.  A
>    26-word hblank packet reloads 8 entries per line with room for the mode /
>    base / skip SETs.  Photographs need 2-5 per line (`clut-experiment`).
>    Consecutive entries could drop the index SET if `clut_index`
>    post-incremented on each write (2 words/entry, 13 per line) — OPEN, costs
>    LCs, not needed for Smurf.

## 5. Bench inventory

| bench | make target | what it covers | result |
|---|---|---|---|
| `pixel_tb.v` | `make pixel-sim` | the decoder: modes, cadence, skip, words/line | **PASS**, 148 cases, 322 checks, 266 line-runs, 0 failures |
| `mutate.py` | `make pixel-mutants` | **9** single-mistake mutants of the decoder | all 9 FAIL as required |
| `timing_tb.v` | `make timing-sim` | the raster laws, against the MERGED top level | **PASS**, 14 checks, 0 failures; all 10 `-DMUTATE=n` controls fail |
| `clut_pair_tb.v` | `make clut-pair-sim` | COMPOSITOR + PIXEL + three RAM models | **PASS**, 11 cases, **65** checks, 0 failures; the `old_pulse` whole-chip mutant FAILS as required |

`mut9` is the write sequence one clock late — `pixel_tb.v` catches it on the
clock numbers alone.  `mutants/old_pulse.v` is the same mistake as a WHOLE-FILE
mutant, because `clut_pair_tb.v` instantiates the merged top level; it is what
`make clut-pair-sim` requires to fail, and it fails on exactly the intended
assertion (`data changed on the /WE RISING edge ... no hold`, in S1c and in both
back-to-back entries of S2).  `mut8` is now "the pulse two clocks wide" against
the new `~wr1`.

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
  committed", which is what the bench now asserts.  (With the pulse moved
  earlier the illegal cases also break setup, so the bench now sees them twice
  over — but the correctness criterion is still the one that matters.)

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

**A REQUIREMENT ON THE MODEL AND THE EMULATOR: model COMPOSITOR's power-on
reset.**  On the board COMPOSITOR's only reset input is TIMING's `/RS`, which
pulses for eight clocks once per frame at the top of the vsync line.  Nothing
resets it at power-up.  What keeps it sane until the first vsync is the
ATF1508AS's own POWER_RESET feature — the `power_reset on` strategy flag every
production fitter target carries — which initialises the macrocell registers when
the supply comes up.  That flag is therefore LOAD-BEARING for COMPOSITOR, not
cosmetic, and the same is true of PIXEL's own `nRESET` only because the board
drives it.

Consequences for packages 2+:

* The model/emulator must apply a power-on reset to the COMPOSITOR unit at
  construction, with the same effect as `/RS`: held colours to 0xFFF/0x000,
  `run_count` terminal, source passthrough, nothing staged, machine clear.
* Whatever does NOT model it sees an **undefined first frame**: everything from
  power-up to the first `/RS` at line 490 runs with COMPOSITOR's state
  unspecified.  `clut_pair_tb.v` has to AND a power-on pulse into `/RS`
  explicitly for exactly this reason, and says so in a comment; without it the
  whole first frame simulates as X and every measurement in it is worthless.
* Firmware and display-list authors get the same warning in different words: the
  first frame after power-up is not a defined picture, and the palette RAM is
  undefined until software writes it.  Nothing in hardware clears either.

---

## 7. Open issues / not done

* **No timing arithmetic.**  The palette READ path (PIXEL tCO + RAM tAA +
  COMPOSITOR tSU inside one 39.72 ns dot clock — the spec draft's 22.7 ns
  budget) and the WRITE path (/WE pulse width, address and data setup/hold,
  write recovery) are section 6's job and are NOT settled here.  Nor are the
  sixteen PIXELS inputs sampled on one GCLK edge.  The pair bench's one-clock
  write recovery is an abstraction, not a datasheet number.
* **The data-hold-at-/WE-rise question is CLOSED on our side** (owner decision,
  2026-09-20): the pulse moved one clock earlier, so the write now has a full
  clock of address AND data setup and a full clock of hold, whatever the part's
  numbers turn out to be.  What is still open is the arithmetic itself — /WE
  pulse width against one 39.72 ns clock, and the actual tAS/tAH/tDS/tDH/tWR of
  the part bought — which is spec section 6's job.
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
