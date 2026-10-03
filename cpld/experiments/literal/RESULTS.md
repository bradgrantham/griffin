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
   after its consume edge.  That gives a stall-free form A (R2s), which has
   the same fan-in problem.  (CORRECTION, Part 2: "simulates correctly" was
   written here without a run.  R2s is not form A's semantics.  It is a
   one-slot record that lands adjacent in a banked pair; Part 2 derives its
   own expectations, FORM_A2, and those pass.)
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

# Part 2: all pins free (2026-10-03, Brad: "just test all pins free")

The question: is block E's fan-in the only thing stopping forms A and B?
Rev 2 is not routed, so the COMPOSITOR pinout may change.  `genpins.py rN
pfree` writes `rN_pfree.v`, which is the rung's logic byte for byte (checked
by diff) with NO `//PIN:` lines.  An empty `.pin` under `-preassign keep` is
free placement.  The flags are otherwise the production set.  `harvest.sh`
writes the fitter's chosen pins to `pins_rN_pfree.txt` (a `//PIN:` block
with the block of each pin, NOT FROZEN) and counts how many moved from rev 1.

New rungs (in `gen.py`):
* `r2st` = R2 + streaming RUN, with count bit 11 low as the flag.
* `r4st` = R4 (form A + HOLD, 10-bit counter) + streaming RUN.  HOLD owns
  bits 11:10 = `0x`, so streaming takes the proposal's reserved `10`
  (`11` RUN, `10` streaming RUN, `0x` HOLD).
* `r4s` = R2s + HOLD + 10-bit counter.
* `r4sst` = `r4s` + streaming.

| rung | what | fits | LC | FF | Nodes+FB | PT | pass | tightest block fan-in | RGB_OUT per block | pins moved (of 62) |
|---|---|---|---|---|---|---|---|---|---|---|
| R2 | form A literal | **yes** | 123 | 93 | 120 | 412 | 1 | 34/40 | E4 F4 G4 | 58 |
| R3 | R2 + HOLD, 12-bit counter | NO | | | | | 5 | | | grouping fail x5 |
| R4 | R2 + HOLD, 10-bit counter | **yes** | 114 | 91 | 124 | 381 | 2 | 35 | F4 G4 H4 | 59 |
| R2st | R2 + streaming RUN | yes | 126 | 94 | 141 | 392 | 2 | 38 | E5 F5 G2 | 58 |
| **R4st** | **R4 + streaming RUN (form A + HOLD + streaming)** | **yes** | **118** | 92 | 119 | 357 | **1** | **35** | D1 E4 F4 G3 | 58 |
| R5 | form B literal (KEEP) | NO | | | | | 5 | | | grouping fail x5 |
| R6 / R6n | R5 + HOLD, 12- / 10-bit counter | NO | | | | | 5 | | | grouping fail x5 |
| R5n | form B without KEEP | yes | 118 | 92 | 116 | 373 | 1 | **40/40** (B, C, D, E) | B3 C3 D3 E3 | 59 |
| R2s | stall-free form A (colour from set_pix_value) | yes | 123 | 93 | 117 | 393 | 1 | 37 | E4 F4 G4 | 59 |
| R4s | R2s + HOLD, 10-bit counter | yes | 120 | 91 | 114 | 396 | 1 | 38 | E4 F4 G4 | 55 |
| R4sst | R4s + streaming RUN | NO | | | | | 5 | | | grouping, placement and routing fails |
| lever R5 + xor_synthesis | | NO | | | | | 3 | | | INTERNAL ERROR |
| lever R6n + xor_synthesis | | NO | | | | | 5 | | | grouping fail |

On a failed free placement the fitter prints no fan-in figure, only "Grouping
fail".  Free placement moves almost every pin (55 to 59 of 62).  That cost is
a property of free placement, not of the logic: see the spread results kept
below.

## Answers

1. **Form A fits with all pins free.**  The bare literal (R2) uses 123/128
   LC, pass 1, with the tightest block at fan-in 34/40.  With HOLD it fits
   only on the 10-bit counter: R4 uses 114/128 (14 LC spare), pass 2,
   tightest 35/40.  The 12-bit R3 fails.  Narrowing the counter matters
   here, unlike on form C.
2. **Form B as proposed (with KEEP, with or without HOLD) does not fit, even
   with all pins free**, and xor_synthesis does not help.  Its RGB_OUT D input
   carries the whole consume decode: the fit report shows a 20-literal product
   term per bit, with H_ACTIVE, the 12-bit terminal and staged_word[15:12].
   Only form B without KEEP (R5n) fits: 118 LC, with four blocks at exactly
   40/40, so zero fan-in spare.  That variant gives the true one-pixel patch
   (BN2: X on slot n-1 only), but a literal stream alternates with the
   background (BN1).
3. **Form A coexists with streaming RUN.**  R4st (form A + HOLD + streaming,
   10-bit counter) uses 118/128 LC (10 spare), pass 1, tightest fan-in 35/40
   (5 spare).  R2st (no HOLD, 12-bit) also fits but at 126 LC, pass 2.  The
   stall-free form A does not take streaming as well (R4sst fails).
4. **Timing depth, as far as the report shows.**  The fitter gives no
   delay numbers or path report, only equations.  In R4st, RGB_OUT_n.D is 5
   product terms in one array pass.  All inputs are registers (the literal's
   bit comes from staged_word's Q) plus RGB_IN.  There is no cascade or
   foldback on the RGB_OUT cells.  Production is 4 terms, same shape, so form A
   adds one term and no logic level.  Form B's term includes the H_ACTIVE pin
   and about 20 literals.  That is still one array level, but it is the deeper
   path the proposal worried about, and it does not fit anyway.

## Simulation (fitting logic rungs; the placement does not change logic)

* lit_tb (all first-run passes against hand derivations):
  * R2st: `-DFORM_A -DSTREAM`.
  * R4st: `-DFORM_A -DHOLD -DSTREAM10`.  STREAM10 is the S1/S2 case with
    the `10` coding.
  * R5n: `-DFORM_BN`.  New cases BN1-BN3: stream `pt 0-2, A 3, B 4, pt 5,
    C 6`; patch `X on n-1 only`; the literal's own slot shows the previous
    source.
  * R2s, R4s: `-DFORM_A2` (`+ -DHOLD` for R4s).  Cases A2_1 to A2_5 and
    A2_H, derived in Part 2.  The literal is a one-slot record: `A 0, B 1-2,
    C 3-4` opening a line; one-pixel patch on slot n; literal after MASK gap
    0; KEEP repeats a mask's last pixel; LIT + HOLD(N) = N + 1 px.  The colour
    registers stay untouched.
  * R2, R3 and R4 still pass `-DFORM_A`.
* Production `compositor_tb.v`, with the same pre-set exclusions (29
  RUN_COLOR-built checks, plus 4 `run_count` width checks on 10-bit rungs):
  0 unexpected failures out of 179 on R2st, R4st, R2s, R4s and R5n.  R2 and
  R4 were done in Part 1.
* Negative controls, all of which fail as required:
  * STREAM10 against R4;
  * FORM_A2 against R2, R8b and R4 (with HOLD);
  * FORM_A against R2s;
  * FORM_BN against R5 and R2;
  * FORM_B against R5n;
  * FORM_A2+HOLD against R2s.

## Earlier Part 2 runs kept as results (before the scope change)

* **P-min** (RGB_OUT released, everything else frozen): the fitter refuses on
  every rung (R2, R3, R4, R4st, R5, R6), with grouping or placement fails.
  This is the known behaviour: under keep, it will not pick pins for
  unassigned ports on a near-full chip.
* **P-spread** (RGB_OUT spread 4/4/4 by swapping pins with VIDCMD_Q inputs;
  RGB_IN does not use block fan-in, and moving it changed nothing, per R2/R3
  `p444i`):

  | regime | R2 | R3 | R4 | R2st | R4st | R2s | R5 | R6 | R6n | R5n |
  |---|---|---|---|---|---|---|---|---|---|---|
  | `p444` | 123, p1, tightest 35 | 124, p1, tightest 40 | 114, p1, tightest 32 | 126, p1, tightest 36 | 118, p2, tightest 35 | 123, p1 | fan-in 41 on D | fan-in 41 on D | fan-in 42 on F | no fit |
  | `p444g` | 123, p1, tightest 35 | routing fail | 114, p1, tightest 34 | 126, p1, tightest 37 | **118, p1, tightest 34** | INTERNAL ERROR | fan-in 41 on E | fan-in 41 on E | fan-in 42 on F | INTERNAL ERROR |
  | `p3333` | placement fail | no fit | 114, p1 | routing fail | no fit | | INTERNAL ERROR | no fit | no fit | |

  `p444` swaps RGB_OUT 3/0/1/2 with VIDCMD_Q 3/2/4/6 (block D pins 39, 40,
  41, 37).  `p444g` swaps RGB_OUT 3/0/1/2 with VIDCMD_Q 10/11/8/13 (block G
  pins 63, 64, 65, 67).  **So form A + HOLD + streaming fits with only 8 pins
  moved (four input/output swaps), not the 58 that free placement moves.**
  Form B fails every spread.

## Recommendation (Part 2)

If the rev-2 pinout may change, **form A + HOLD + streaming RUN (R4st) is
adoptable**: 118/128 LC, pass 1, at least 5 fan-in spare.  Adopting it means
narrowing RUN and HOLD counts to 10 bits (1023 max, as the proposal's rule 6
says).  The minimal pin change found is `p444g`'s four swaps.  Free
placement's table (`pins_r4st_pfree.txt`) is a valid fallback but moves 58
pins.  Form B with KEEP does not fit any pinout tried.  Form C (Part 1) stays
the only option that keeps the frozen rev-1 pinout.  Adopting R4st is a
semantic change through all five video-chain layers.  The lit_tb FORM_A,
HOLD and STREAM10 cases are the starting point for the production bench.
