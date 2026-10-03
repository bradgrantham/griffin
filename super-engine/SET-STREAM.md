# SET stream: literal pixels on today's COMPOSITOR?

Host-only experiment in `super-engine/`: trace block **`m13-set-stream traces`**
and frame case **`m13-set-stream`** (`make check`).  Nothing under `cpld/`,
`emulator/`, `firmware/`, `apps/` or `audio/` was touched.

## The question

Take a run of `SET cmp_color1` words (`{1, 000, value12}`) played while the
current source is `cmp_color1`.  Does each word put one arbitrary 12-bit colour
on screen for two slots, so we get a "literal pixel" stream with no CPLD
change?  If so, what are the exact rules?

## Answer

**Yes, with conditions.**  Each SET paints its own slot plus the one HOLD slot
behind it (F1 below), so in steady state the stream is one 12-bit pixel per
word at 320 density.  Every derived picture below matched the model pixel for
pixel on the first run: 18 single-line traces, and three full-frame variants
over 307,200 pixels × 2 frames, checked with both the clocked driver and the
emulator's line-at-a-time driver.

The conditions:

1. **Lead-in.**  You need one extra word up front:
   `[SET c1 = first colour, RUN(c1, 2)]`, placed as the banked pair behind a
   record of at least 3 slots.  A bare `RUN(c1, n)` followed by the SETs either
   makes the first pixel one slot wide (n ≥ 2) or shows a stale slot (n = 1).
2. **Geometry costs one word per pixel, plus one invisible word after every
   record of 3 or more slots.**  A one-pixel hole between two colours needs the
   colour after the hole to be preloaded in the other register.  Some patterns
   cannot be drawn at exact geometry at all.
3. **x = 0 and x = 2 cannot start with a full-width pixel.**  A sprite at the
   left edge gets a one-slot first pixel.
4. **`cmp_color1` is clobbered.**  `cmp_color0` is clobbered too if the stream
   uses it.
5. **Delivery.**  The stream drains words 1.8× faster than ENGINE delivers them.
   Either the whole program is in the FIFO before the sprite starts, or at least
   `≈ 0.44·W + 2` of its W words are.  A late word shifts the picture: it does
   not keep the geometry.

## The three facts (from vidcmd-spec.md §4.1, compositor.v's header)

* **F1 (L4).**  In steady state a word costs two slots.  A one-slot record
  paints its own slot, then a HOLD slot that keeps the source.  A SET does not
  change `cur_src`, so the HOLD shows the colour the SET just committed.
  Each SET is therefore two slots of one colour.
* **F2 (L5, the banked pair).**  Call it a *cadence entry* when a record of
  n slots is consumed with nothing parked behind it, i.e. the record was itself
  captured on the edge before.  With n ≤ 2 the next record lands at +2.  With
  n ≥ 3 the record builds a pair: the next record lands on its terminal slot
  and the one after that a single slot later.  Call it a *banked entry* when the
  record is a line's first record, or the first half of a pair.  A banked entry
  builds the pair already at n ≥ 2.  (This is the "≥ 4 px of playback in front"
  rule: a 3-slot run plus the record before it.)
* **F3 (consequence).**  Exact 2-slot geometry means **exactly one word per
  pixel** in a stream.  After any record of ≥ 3 slots there is one extra word,
  the first half of the pair.  It gets one slot and must be **invisible**: a SET
  of the register that is not on screen, a SET while passthrough is showing, or
  the target-7 filler.  Dropping it does not work: the next pixel would then be
  one slot wide.

Every trace expectation below was derived from these three facts before the
model ran.

## (a) The basic stream

All traces start with `RUN(pass,100)` banked at slot 0.  The colours are
`c0..c3`.  The table gives the slots each colour occupies.

| program after `RUN(pass,100)` | derived = measured | cadence holds |
|---|---|---|
| `RUN(c1,4)`, 4 SETs | 100-103 stale 0xFFF, **c0 on 104 only**, c1 105-106, c2 107-108, c3 109-110, pass 111 | 3 |
| `RUN(c1,1)`, 4 SETs | **100 stale**, c0 101-102, c1 103-104, c2 105-106, c3 107-108, pass 109 | 4 |
| **`SET c1=c0`, `RUN(c1,2)`, 3 SETs** | c0 101-102, c1 103-104, c2 105-106, c3 107-108, pass 109 | 3 |
| `SET c1=c0`, `RUN(c1,1)`, 3 SETs | identical picture | 4 |

* Behind a banked `RUN(c1, n ≥ 2)` the first SET is the first half of a pair.
  It gets **one slot**, and K SETs occupy 2K − 1 slots.  With n = 1 there is no
  pair, but the RUN's own slot shows whatever `cmp_color1` held before.
* **Lead-in rule.**  Spend the pair on `[SET c1 = C1, RUN(c1, 2)]`.  The SET sits
  on the last background slot, under passthrough, so it is invisible.  The RUN is
  a cadence entry with n = 2, so no new pair forms.  For a sprite starting at x0
  behind a record ending at slot x0 − 1: pixel i occupies slots x0 + 2i and
  x0 + 2i + 1, and the K-pixel stream occupies exactly **2K slots**.
  `RUN(c1,1)` also works but adds a cadence hold.  `RUN(c1,3)` would build a
  pair (F2).
* The record in front must be ≥ 3 slots (cadence entry) or ≥ 2 slots (banked:
  line start, or the first half of a pair).  Sprites on one line therefore need
  ≥ 4 slots (2 pixels) between them, counting the SET's slot: exit `RUN(pass, m ≥ 3)`.

## (b) Framing

Occupancy is the authored sum plus one HOLD per SET that runs at cadence.
A one-slot SET plus its HOLD is two slots, so a 16-pixel sprite is
`1 (SET, counted as a background slot) + 2 (RUN) + 15 × (1 + 1) = 33` slots
from the lead-in.  That closes as `X + 1 + 32 + R = 640`.

The frame case authors such lines and measures: **640 slots exactly on every
line**, starvation holds 0, overruns 0.  Cadence holds come to exactly the
derived count: 15 for S1 + 8 for S2 = 23 per band line × 32 lines × 2 frames
= 1472.

**A cadence hold between SETs is the mechanism, not a fault.**  It is the
second slot of every pixel.  `vidcmd_plan_line()` agrees: 640 slots.  Its
`stretch` reads 23 (resp. 15) only because its authored sum counts a SET as one
slot.

## (c) Transparency

Measured traces, after the lead-in at x0 = 101:

| pattern | words | derived = measured |
|---|---|---|
| C C T C (A B · D), `cmp_color0 = D` preloaded in HBLANK | `SET A`, `RUN(c1,2)`, `SET B`, `RUN(pass,2)`, `RUN(c0,2)` | 4 words / 4 px.  The stream then continues in `cmp_color0` (SET target 1) |
| C C T C with D = B | `… SET B`, `RUN(pass,2)`, `RUN(c1,2)` | 4 / 4, no preload |
| C T T C (A · · D) | `… RUN(pass,3)`, `SET c1=D`, `RUN(c1,2)` | `RUN(pass,3)` builds a pair; the SET hides under passthrough.  3 words for T T D |
| A A A B, then T, then X | `SET A`, `RUN(c1,5)`, `SET c0=X`, `SET B`, `RUN(pass,2)`, `RUN(c0,2)` | the run's pair gives a free invisible slot, used to preload X |
| **C T C, nothing preloaded** | `… RUN(pass,2)`, `SET c1=D`, `RUN(c1,2)` | **the hole is 2 px wide and D is 1 px late.**  Asserted as the derived wrong picture |

Rules:

* **A hole of p ≥ 2 pixels** is `RUN(pass, 2p − 1)` followed by
  `[SET R = next colour, RUN(R, 2)]`: 3 words for p + 1 pixels.
* **A one-pixel hole** is `RUN(pass, 2)` then `RUN(R, 2)`, where register R
  already holds the colour after the hole.  That colour is in `cmp_color1` if it
  equals the pixel before the hole.  Otherwise it must have been preloaded into
  the other register.  `RUN_COLOR` also works if the colour is one of the eight
  corners.
* **Preloads are free in exactly these places:**
  * HBLANK (eager SETs);
  * any repeated pixel, since its word can be a SET of the off-screen register;
  * the pair slot behind any hole or same-colour run of ≥ 2 pixels.

  Each preload serves one re-entry.  After re-entering on `cmp_color0` the
  stream continues on target 1, and `cmp_color1` becomes the off-screen register.
* **Words per pixel.**  It is always 1 word per source pixel, plus 1 per sprite
  for the lead-in, plus the exit/gap RUN.  A run of ≥ 3 same-colour pixels or
  holes costs 3 words whatever its length.  There is no worst case that costs
  more words.  The worst case is a pattern that **cannot** be drawn: a one-pixel
  hole whose right-hand colour is new, with no free preload left.  Example:
  `A · B · C` with three new colours.  Each extra word shifts everything after
  it by 2 slots.

## (d) Exit

The last SET executes at x0 + 2K − 2 and its HOLD at x0 + 2K − 1.  The exit
`RUN(pass, R)` executes on **x0 + 2K**.  Passthrough resumes immediately after
the last pixel's two slots, with **no stale slot**.  This is measured in every
trace.

## (e) Starvation

The trace pushes `SET D`, `SET E` and the exit at slot 140.  C had executed on
105 with the FIFO dry.

* 106..139 are **starvation holds** (34) that keep C on screen.
* The /EF synchronizer is cold, so the word falls on 142, is captured on 143,
  and **D executes on 144 = P + 4**.  E executes on 146, the exit on 148.
  The line overruns.
* Result: **the colour stretches and everything after it shifts.**  The
  geometry is not kept.  Measured: 37 slots late, 34 starvation holds, 7 cadence
  holds, 1 overrun.

Frame level (variant `late`): line 200's last two words (SET A15 and the exit)
are deposited after the line's 80 PIXELS words.  The words arrive at slots 302
and 306, read from the ENGINE model's deposit cycles.  The law says A15
executes on P0 + 4 = 306.  The exit's push missed the synchronizer, so it goes
through the cold path from P1 and executes on P0 + 8 = 310.

The model matches that pixel for pixel, and **the shift (279) persists on every
later line until `/RS`**:
* 280 overruns per frame;
* the band below line 200 is drawn 279 slots to the right;
* starvation holds and cadence holds are exactly the derived counts.

Two gotchas:

* **A word that is late only by the synchronizer is counted as a *cadence*
  hold, not as starvation.**  This is trace g-9 in (g): 0 starvation holds,
  a 4-slot shift.  Only the overrun, or exact-640 framing, exposes it.  A
  checker that trusts `vidcmd_hold_slots == 0` alone will miss it.
* The emulator's driver lumps each line's deposits into its HBLANK, so it
  cannot show mid-line lateness at all.  The `late` variant is checked with the
  clocked driver only.

## (f) The registers

* The stream writes target 0 only, so **`cmp_color0` is untouched** unless the
  author uses it for preloads or hole re-entries.  Trace: a preset
  `RUN(c0,10)` after the stream still shows its colour.
* **`cmp_color1` is left holding the stream's last colour.**  A later
  `RUN(c1, n)` on the same line shows that colour (asserted).
* **Repair:** re-SET `cmp_color1` as the invisible pair word in front of the
  RUN, and shorten the passthrough RUN before it by one (asserted).  That costs
  1 word.  On the next line, the eager HBLANK SET reloads it for free.
* The same applies to MASK records with `cmp_color1` / `cmp_color0` dibits.
  This is derived, not traced; vidcmd-spec.md §4.4 gives the seam costs of a SET
  between mask groups.

## (g) Bandwidth and delivery

**Words.**

* A 16-pixel row is 17 words (lead-in SET, `RUN(c1,2)`, 15 SETs) plus 1 exit/gap
  RUN, i.e. **18**, opaque or with transparency (S2 is also 17 + 1).
* A line is 1 + 18·S words, plus eager preloads.  Two sprites with one `cmp_color0`
  preload come to **37 words**.

| backdrop | + 2 sprites | vs 147-word cap | most sprites |
|---|---|---|---|
| 40-word attribute / 1bpp | 77 | fits | 5 (133); 6 is 151 |
| 80-word 4bpp half rate | **117 (measured)** | fits | 3 (135); 4 is 153 |

The measured `k1-two` group (80 PIXELS words in 3 descriptors, plus 37 VIDCMD
words in 2) costs **310 SYSCLK**.  That is over the 294-SYSCLK (66%) figure
because of descriptor turn-around, and well inside the 445-SYSCLK line.
Geometry is not the limit: 640 slots holds about 17 sprites with 4-slot gaps.

**Per-word deadline (derived, then measured):**

* In a running stream, word j must be in the FIFO by **3 slots before** its
  execution slot (the synchronizer's meta stage on the capture edge).
* After a dry spell the deadline is **4 slots** (P + 4).

**How much must be buffered.**  The stream consumes a word every 2 pixel clocks.
ENGINE delivers one every 2 SYSCLK, which is 3.596 pixel clocks.  Suppose ENGINE
keeps delivering in step from the moment the lead-in executes.  Then a W-word
sprite needs B words already buffered, where

  `B ≥ (1.596·W + 7) / 3.596`

That is **10 of the 18 words** for a 16-pixel row.  Trace g: B = 10 lands as
authored, with the last word one slot inside its deadline.  B = 9 misses
SET c15 by one slot: c14 and c15 each get 4 slots, the exit moves 4 slots late,
and the line overruns.

Any other traffic on the bus in the meantime (PIXELS descriptors) means nothing
arrives, so then **B = W**.

**Practical rule:** have the line's whole program in the FIFO before pixel 0.
There are two ways:

* **k = 0, VIDCMD first in the line's own HBLANK group.**  PN7 allows
  `2W + 23 ≤ 80` SYSCLK, so **W ≤ 28**: one sprite (18 words) fits, two (37) do
  not.
* **k = 1, one line of lookahead** (STATIC-PIECES.md), which carries two
  sprites easily.

**The left edge.**  A line's first record is always banked, so the landing slots
are {0, 1, 3, 5, …} or {0, n, n + 1, n + 3, …}.

* **x = 0 cannot have both pixel 0 and pixel 1 two slots wide**, and x = 2 is
  impossible too.  x = 1 works with an eager SET + `RUN(pass,1)` + `RUN(c1,2)`.
  x ≥ 3 works with the ordinary lead-in.  x = 0, x = 1 and x = 3 are traced; x = 2 is derived from the landing-slot set (a pixel at 2 needs the next record on 4, which is not a landing slot).
* The x = 0 form is an eager SET + `RUN(c1,1)`: pixel 0 on slot 0 only, then
  2-slot pixels.  In other words, the sprite drawn at x = −1 and clipped.

Frame variants for the left-edge sprite:

| variant | delivery | result |
|---|---|---|
| `k0-vc` | program first in its own HBLANK (18 words) | **0 wrong pixels**, 0 starvation, 0 overruns, worst group 258 SYSCLK |
| `k0-px` | program behind the line's 80 PIXELS words.  Derived first VIDCMD word ≈ 211 SYSCLK after the HBLANK edge, about 120 SYSCLK past pixel 0 | **starves**: 4,085 wrong pixels, 440 starvation holds, 960 overruns (every line of both frames runs late).  This is the negative control |
| `k1-two` | both sprites, one line ahead | **0 wrong pixels**, both drivers.  A one-slot slip of S2 is caught (480 pixels) |

## What it means for multi-colour sprite rows, today versus LITERAL

Today you can author a 320-density, 12-bit-per-pixel sprite row at
**18 words per 16 pixels**, about what vidcmd-literal-hold-proposal.md §4
budgets for LITERAL (~16).  So **LITERAL buys no bandwidth for sprite rows**.
Its real gains are in authoring:

* **Arbitrary one-pixel holes.**  `LIT A`, `RUN(pass,2)`, `LIT C` needs no
  register.  Today such a hole needs a preload, and `A · B · C` cannot be drawn.
* **No register clobber.**  Today the stream eats `cmp_color1`, and
  `cmp_color0` if used, so single-colour RUN spans and MASK colours on the same
  line need re-SETs.
* **Fewer special words.**  No lead-in word and no "invisible word after a
  ≥ 3-slot record".  (The proposal still compiles rows for a "≥ 4-px run in
  front" entry state, so some pair rule survives there too.)
* **Cheaper same-colour runs.**  `LIT + HOLD(N)` is 2 words, against 3 today.

LITERAL also costs something:

* the logic fit is unproven;
* RUN_COLOR goes away.  Today RUN_COLOR is the free third register for corner
  colours;
* every RUN_COLOR list must be re-authored.

Either way, the left edge has the same banked-start parity issue.

**Recommendation:** an offline row compiler that applies the rules above
(lead-in, one word per pixel, hole and preload planning, invisible pair words)
can ship sprites today.  Re-evaluate LITERAL only if real art turns out to be
dominated by isolated one-pixel holes, or by register conflicts with MASK and
RUN spans on the same lines.

## Running it

```
cd super-engine && make check     # m13 is the last block
```

* Artifact: `m13-set-stream.ppm` (k1-two, frame 2).
* Code (all in `main.cpp`):
  * `check_set_stream_traces()` (the traces);
  * `ss_trace()` (trace driver with timed mid-line deposits);
  * `run_set_stream_case()`, `ss_line_program()` and `ss_author()` (frame case);
  * `ss_late_positions()` (the late-word law applied to deposit cycles).
