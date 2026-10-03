# LITERAL / HOLD fit ladder: COMPOSITOR (cpld/experiments/literal/)

FIT EXPERIMENT, 2026-10-03.  Only this directory was touched: no production
Verilog, no cpld/Makefile, no pin file, no bench edits, no commit.  The
proposal under test is `vidcmd-literal-hold-proposal.md`.

Tool flow and flags are the production `compositor:` target's, verbatim:
`run_yosys.sh <rung>`, then `run_fitter.sh <rung> -preassign keep -strategy
debug = on -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy
power_reset on -strategy JTAG = on`.  The part is an ATF1508AS PLCC84, tpd 15.
`make rN` builds a rung.  Each run is bounded by
`perl -e 'alarm 900'`; no run came near it (about 7 s each).  Every rung
carries the production `//PIN:` block unchanged, and `run_fitter.sh` writes
the pin file from it.  `make pins` checks that every fitting rung's output
`.pin` has all 62 frozen signal pins on their frozen numbers.  It does.

Sources: `r0.v` is a byte copy of `cpld/compositor/compositor.v`.  `gen.py`
emits every other rung from `r0.v` as a list of exact text edits, each of which
must match exactly once, so no rung can drift from the production source.
`rNx.v` is a byte copy of `rN.v` fitted with `xor_synthesis` added (a lever,
not the production strategy set).

## Ladder (ATF1508AS PLCC84, frozen pinout, production flags)

| rung | what | fits | LC /128 | FF | Nodes+FB | PT | pass | block E fan-in | failure |
|---|---|---|---|---|---|---|---|---|---|
| R0 `r0` | production compositor.v, refit | yes | 122 | 95 | 143 | 389 | 2 | 39/40 | (pass 1 grouping fail, as shipped) |
| R1 `r1` | RUN_COLOR removed, src 11 reserved | yes | 121 | 92 | 121 | 370 | 1 | 36 | |
| R1h `r1h` | R1 + HOLD(N) alone (KEEP = RGB_OUT feedback), no literal | yes | 118 | 92 | 117 | 379 | 1 | 40 | |
| R2 `r2` | form A LITERAL (bank held for its slot, then KEEP) | **NO** | | | | | 5 | 45 needed | grouping fail x5: "Unable to Keep Fanin of 45 on Block E" |
| R2s `r2s` | form A, colour read from set_pix_value, no bank stall | **NO** | | | | | 5 | 44 | same, fan-in 44 |
| R3 `r3` | R2 + HOLD(N), 12-bit counter | **NO** | | | | | 5 | 45 | same, fan-in 45 |
| R4 `r4` | R3 with a 10-bit run counter | **NO** | | | | | 5 | 44 | same, fan-in 44 |
| R5 `r5` | form B LITERAL (RGB_OUT loads on the consume edge, then KEEP) | **NO** | | | | | 5 | 41 | same, fan-in 41 |
| R5n `r5n` | form B with no KEEP (the literal's own slot keeps the old source) | **NO** | | | | | 5 | 41 | same, fan-in 41 |
| R6 `r6` | R5 + HOLD(N), 12-bit counter | **NO** | | | | | 5 | 41 | same, fan-in 41 |
| R6n `r6n` | R6 with a 10-bit run counter | **NO** | | | | | 5 | 44 | same, fan-in 44 |
| R7 `r7` | streaming RUN on R0 (count bit 11 low lets SETs through), no literal | yes | 126 | 96 | 133 | 401 | 1 | 39 | |
| R8 `r8` | **form C** LITERAL = SET cmp_color0 + select colour0 | **NO** | (126) | | (140) | | 5 | | grouping fail x4, then routing fail |
| **R8b `r8b`** | **form C LITERAL = SET cmp_color1 + select colour1** | **yes** | **120** | **92** | **119** | **386** | **1** | 38 | |
| R8bh `r8bh` | R8b + HOLD(N) (KEEP) | **NO** | | | | | 5 | | grouping fail x5 |
| R8bn `r8bn` | R8b with a 10-bit run counter | yes | 124 | 90 | 119 | 396 | 1 | 37 | |
| R9 `r9` | R8b + streaming RUN | **NO** | | | | | 5 | | grouping fail x5 |
| lever `r2x` | R2 + xor_synthesis | NO | | | | | 5 | 45 | fan-in 45 on block E |
| lever `r5x` | R5 + xor_synthesis | NO | | | | | 5 | 41 | fan-in 41 on block E |
| lever `r5nx` | R5n + xor_synthesis | NO | | | | | 5 | 41 | fan-in 41 on block E |
| lever `r8x` | R8 + xor_synthesis | NO | | | | | 5 | | grouping x3, routing fail |
| lever `r8bhx` | R8bh + xor_synthesis | NO | | | | | 5 | | grouping fail |
| lever `r9x` | R9 + xor_synthesis | NO | | | | | 1 | | fitter INTERNAL ERROR on pass 1 |

R0 reproduces the shipped `compositor.fit` exactly: 122 / 95 / 143 / 389, with
the same pass-1 grouping fail and pass-2 fit.  `diff` against
`cpld/compositor/compositor.fit`, with the design name and timestamp
normalised, shows one reordered pair of equation lines and nothing else.  The
flow is therefore the production flow.  The block E fan-in column is the
`(NN)` field of the `E:` row in each `.fit`.  A blank cell means the fitter
reported no usable number.  A number in parentheses is the fitter's partial
count on a failed pass.

## Why forms A and B cannot fit this pinout

**The limit is block E's 40-signal fan-in, and the frozen pinout fixes
it.**  Product terms and macrocells are not the problem.  Block E (LC65-80)
holds eight of the twelve RGB_OUT pins (44, 45, 46, 48, 49, 50, 51, 52 =
RGB_OUT 7, 8, 5, 4, 3, 0, 1, 2).  Production already brings 39 of block E's
40 signals in: eight RGB_IN, eight cmp_color1, eight cmp_color0 bits (some as
helper nodes), plus cur_src, h_active_d and cur_colour (`r0.fit`, "FanIn
assignment for block E [39]").  Any new source with a DIFFERENT signal per
colour bit costs eight more signals in block E.  The literal's colour is such
a source: staged_word[11:0] in form A, set_pix_value[11:0] in R2s, and the
direct D-input in form B.  Removing RUN_COLOR frees only the three cur_colour
signals (R1: 36).  36 + 8 = 44, more than 40, whatever the form.  The
fitter's own counts agree: 41 to 45 on every A/B rung.

* Neither the bank stall nor KEEP is what fails.  R5n (form B with no KEEP
  and no stall) fails at the same 41.  R1h (KEEP feedback alone) FITS, at
  118 LC.  KEEP costs block E one signal (36 -> 40), not eight.
* The 10-bit counter, HOLD and xor_synthesis do not move the fan-in.  Every
  lever run fails with the same message.
* So no LITERAL that reaches RGB_OUT through a new per-bit path can be added
  to COMPOSITOR without moving RGB_OUT pins out of block E, and that is a
  respin.  This holds for the whole family of "+1 PT per RGB_OUT cell"
  designs, not just the two forms tried.

## Form C: what does fit

Form C is a new form; it is not in the proposal.  **A LITERAL writes its colour
into cmp_color1 and selects colour1, in one word.**  The encoding is the
proposal's `{00, 11, colour[11:0]}`.  The colour lands in a register that
already feeds the RGB_OUT mux, so the mux and block E gain nothing.  In the
RTL (`r8b.v`):

* `w_is_literal = w_is_run & (w_src == 2'b11)`
* `write_color1 = (apply_set & target == 0) | load_literal`
* `cur_src <= w_is_literal ? SRC_COLOR1 : w_src`
* The counter is not loaded on a literal.  It is terminal on the consume edge
  and stays terminal.

The literal is therefore an ordinary one-slot playback record.  L1-L5, the
fetch engine, /RS and eagerness are all unchanged.  A banked pair lands on
adjacent 640-resolution slots.

**Fit: 120/128 LC, 92 FF, 119 Nodes+FB, 386 PT, pass 1, 8 LC spare.**  That
is 2 LC and 3 PT below production.  **The margin is thin and depends on
fitter luck**: the mirror-image R8 (into cmp_color0) has the same logic and
fails grouping and routing.  R8b with HOLD or with streaming RUN also fails.
The 10-bit counter does not help (R8bn fits, but at 124 LC, worse).  Treat
R8b as "fits as written", not as "has 8 LC to spend".

### Seam constants (lit_tb.v, R8b; derived first, then simulated; all matched)

| quantity | value |
|---|---|
| literal pitch, sustained 2-slot cadence | 2 slots (literal + HOLD of colour1), C1/C2 |
| literal pitch inside a banked pair | 1 slot: two literals on adjacent pixels, C1 |
| slot after a literal | +1 slot if the pair is banked, else +2 (the same as any one-slot record) |
| RUN(pt, 8), LIT, RUN(pt) | a true one-pixel patch: X on slot 8 only, pt from 9 (C3) |
| flat span: LIT X + RUN(colour1, N) | N + 1 px (C4: N = 5, slots 4-9) |
| literal after MASK | gap 0: X on slot L+16; mask pixel 15 keeps the old colour1 (C5) |
| MASK after a literal | ordinary cadence; the mask's colour1 dibits now show X, and colour1 = X resumes after the mask (C6) |

### What is traded away (form C against the proposal)

* **cmp_color1 is clobbered by every literal.**  A MASK that uses
  colour1 dibits after a literal needs a SET cmp_color1 first, or shows the
  literal colour.  The implicit opaque pixel 0 of a mask is colour0, so it is
  not affected.  SET target 0 and LITERAL write the same register.
* No KEEP and no HOLD(N).  The proposal's "LITERAL + HOLD(N)" becomes
  "LITERAL + RUN(colour1, N)", also two words, one pixel narrower (N + 1
  against form A's N + 2).  "Keep the last output colour after passthrough"
  (smear, rule 5) does not exist.  It only fits without a literal (R1h).
* RUN_COLOR is gone, as in the proposal: the one-word span in the eight corner
  colours.
* Count bit 11 stays free: no RUN ever sets it, and R8b keeps the 12-bit
  counter.  It cannot also hold streaming RUN on this chip (R9 does not fit).

### Other measured seams (forms A and B, which do not fit; for the record)

| | form A (r2-r4) | form B (r5, r6, r6n) |
|---|---|---|
| LIT A B C after RUN(pt,4) | A 4-5, B 6-7, C 8-9, pt 10 (pitch 2, even inside a banked pair) | pt 0-2, A 3, B 4-5, C 6-8, pt 9 |
| LIT A B C opening the line | A 0-1, B 2-3, C 4-5, pt 6 | **A is lost** (registered into slot -1, which is blanked), B 0-1, C 2-4, pt 5 |
| RUN(pt,8) X RUN(pt) | X 8-9, pt 10 | **X 7-8, pt 9** |
| literal after MASK | gap 0: X on L+16, L+17 | X overwrites mask pixel 15: X on L+15 to L+17 |
| LIT X + HOLD(5) after RUN(pt,4) | X 4-10 (N + 2) | X 3-9 (N + 2, one slot early) |
| MASK after LIT, nothing after | KEEP repeats the mask's last pixel (rule 4 confirmed) | (not run) |

## Streaming RUN (R7, Brad 2026-09-21)

On R0, a RUN with count bit 11 low sets one flag FF.  While that RUN counts, a
staged SET is consumed at once (`consume_active` gains `streaming &
w_is_set`).  RUN_COLOR is excluded because its bit 11 is a colour bit.
**Fits: 126/128 LC (+4), 96 FF (+1), 401 PT (+12), pass 1, 2 LC spare.**
Simulated (S1): with RUN(colour1, 20, streaming) and SETs A, B, C behind it,
the SETs land on slots 1, 3 and 5 while the run keeps counting, and the next
RUN starts on slot 20 exactly.  S2 checks that the non-streaming RUN still
holds the SETs to its terminal.  The full production bench passes on R7 with
nothing excluded.  It does NOT fit together with form C (R9, nor R9x, which
crashes the fitter).  Streaming RUN and LITERAL are therefore alternatives on
this chip, not partners.

## Simulation honesty

* `make sim` runs `lit_tb.v` on every rung it applies to (A, B, C, HOLD,
  streaming, HOLD-only).  Every expectation was written down from the laws
  before the first run, and every one matched on that first run.
* `make regress` runs the PRODUCTION `cpld/compositor/compositor_tb.v`,
  unmodified, against each rung.  `-DLIT_TB` exposes only a constant
  `cur_colour` for its one probe.  The exclusions were set before running.
  They are all 29 checks of the five RUN_COLOR-built cases (RUN_COLOR*,
  ONE_PX_SPANS, SUSTAINED_2SLOT, RLE_TRUNCATION, MASKB_STARVE).  On 10-bit
  rungs they also include the four checks that compare `run_count` with a
  12-bit literal.  Result: 0 unexpected failures on every rung, out of 179
  checks.  R0 and R7 run with no exclusions.
* `make negative`: each form's expectations run against the other forms'
  rungs, STREAM against R0, HOLD-only against R1, and HOLD against the rungs
  without it.  All 10 fail, as they must.  Three single-mistake mutants of
  R8b (`mutate.py`: literal does not write colour1; literal does not select
  colour1; literal loads its colour as a count) all fail.

## What in the proposal turned out wrong

1. **The cost model.**  "About +1 PT per RGB_OUT cell" is not what binds.  The
   binding limit is block E's fan-in, which is 39/40 in production with eight
   RGB_OUT pins frozen in that block.  Any per-bit colour path is +8 signals.
   Forms A and B cannot fit on this pinout, with or without HOLD, the 10-bit
   counter or xor_synthesis.
2. **Form B's one-pixel patch is wrong by one.**  A form-B literal still
   occupies its own consume slot (L1), and KEEP paints it.  It ALSO
   overwrites the slot before.  `RUN(pt,n) LIT RUN(pt)` gives X on n-1 AND n,
   with passthrough from n+1.  The claim "occupies no slot of its own" is
   wrong.  The one-pixel patch needs B without KEEP (R5n), and then a literal
   stream alternates with the background instead of tiling.  B with KEEP and
   B without KEEP cannot both be had.  A form-B literal that opens a line is
   lost entirely, not smeared.
3. **"No 12-bit latch exists to hold the colour."**  Two do: cmp_color1 and
   cmp_color0 already feed the mux (form C), and set_pix_value registers
   staged_word on every edge, so it holds a literal for exactly the slot
   after its consume edge.  That gives a stall-free form A (R2s), which
   simulates correctly but has the same fan-in problem.
4. **"About -5 FF."**  Removing RUN_COLOR frees 3 FF and 1 LC (R1: 121 / 92 /
   370 PT).  Narrowing the counter frees 2 more FF but costs LC (R8bn: 124
   against R8b's 120).  Rung 3's narrowing buys nothing on this chip.
5. Form A's measured seams match its own derivations: pitch 2 everywhere,
   +2 after a literal, rule 4 after a MASK.  But it does not fit.

## Recommendation

* **Forms A and B (the proposal as written): not adoptable on COMPOSITOR with
  the frozen pinout.**  Revisit only at a respin that moves four of the eight
  block-E RGB_OUT pins elsewhere.
* **Form C (R8b) is adoptable**: 120/128 LC, pass 1, frozen pinout, production
  flags.  It keeps the proposal's encoding and most of its uses: true-colour
  sprite rows at 320 density, two-word any-colour spans, video-window
  literals, and one-pixel patches at 640 resolution in a banked pair.  The
  costs are colour1 clobbering and no KEEP/HOLD.  Because the R8 mirror fails,
  any later RTL change to R8b must be re-fitted before anything else.
* Streaming RUN (R7) fits on its own at 126/128.  The choice between it and
  form C is Brad's; both together do not fit.

## What adopting form C would take (NOT done)

1. RTL: `compositor.v` takes the R8b edits.  Header: instruction table
   (RUN_COLOR -> LITERAL), a LITERAL law section (one-slot playback record,
   writes cmp_color1 and selects it, entry-edge commit, waits for H_ACTIVE
   like a RUN, MASK interaction), and the KEEP-free source list.  The pin
   block is unchanged.  Re-fit before anything else.
2. `compositor_tb.v`: replace the five RUN_COLOR cases (29 checks) with
   literal cases ported from `lit_tb.v` C1-C6, and add the measured constants
   (pair-adjacent literals, +1/+2 after a literal, gap 0 after a mask).
   Negative controls like `mutate.py`'s.
3. `super-engine/render.{h,cpp}` CompositorUnit: decode src 11 as "write
   cmp_color1, select colour1, no count".  `descriptor.h`: a literal encoder
   replaces the RUN_COLOR one.  The `plan` recurrence does NOT change: a
   literal is a one-slot record like a SET.
4. Suite: re-author m7-kiosk's highlight bar, and add a literal case (sprite
   row, patch, flat span) asserted pixel-exact.
5. Emulator: inherits through render.cpp; rebuild clean.  Then the
   RUN_COLOR users: apps/dtest, apps/smurf + image-tools/sprite2spans,
   apps/dmabench, the griffin-ports Smurf borders, vidcmd-spec §3/4,
   griffin.yml constants, the datasheet's LITERAL section and status stamp.
