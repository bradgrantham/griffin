# Static pieces: can ENGINE assemble a sprite line's VIDCMD program?

Host-only design experiment, `super-engine/`, suite case **`m11-static-pieces`**
(`make check` in `super-engine/`).  Nothing under `cpld/`, `emulator/`,
`firmware/`, `apps/` or `audio/` was touched.

## The question

Today (`apps/smurf`) a sprite line is **one descriptor of 21 words** delivered in
that line's own horizontal blanking.  The CPU copies the fragments — border,
gap, the pose row's ready words, tail, border — into one contiguous program in
scratch RAM and pads it to a frozen 21-word shape.  The deadline derivation that
froze the shape was `20 + 2W + 15 <= 78 SYSCLK` ⇒ `W <= 21`.

The proposal: **five descriptors per sprite line, each pointing at data that is
never written after start-up.**

```
piece 1  2 words   left cap:  SET(pix_base, b) + RUN_COLOR(black, 64)
piece 2  1 word    gap:       RUN(passthrough, g)   from a 640-entry table
piece 3  8 words   the pose row, straight out of the art
piece 4  1 word    tail:      RUN(passthrough, t)   from the same table
piece 5  1 word    right border: RUN_COLOR(black, 64)
```

Five descriptors cost four extra ENGINE turn-arounds, which breaks the
one-hblank deadline, so the pieces must be deposited **k lines ahead** into the
256-word VIDCMD FIFO.

## Answer

**It works, with four conditions.**  The minimum clean lookahead is **k = 1**,
and k = 1, 2, 4 and 8 all render **pixel-identical** to the same lines authored
as single padded 21-word programs — 0 differing pixels out of 307,200, on both
rendered frames.

The four conditions:

1. **Lookahead is a uniform pipeline offset, not a per-line choice.**  The VIDCMD
   FIFO is one ordered stream, so line *y*'s program cannot be deposited early
   unless every program before it is deposited at least as early.  Every line's
   program moves k groups earlier, plain lines included.
2. **Inside a group the order inverts: PIXELS first, pieces second.**  At k = 0
   the VIDCMD words belong to the imminent line and must go first; at k > 0 they
   do not, and PN7's PIX_PRELOAD deadline for the pixel stream is the tight one.
3. **The bootstrap must land after `/RS`.**  Lines 0..k−1 have no group to be
   deposited in and go in vertical blanking, *after* TIMING's vsync `/RS` pulse
   — which empties both video FIFOs.  This is the one place where *when* a word
   is deposited is not reducible to its order, and it is the third negative
   control below.
4. **Framing must be planned over the semantic prefix.**  `vidcmd_plan_line()`
   has no H_ACTIVE fall, so it plays trailing pads as active records; it reports
   654 slots for the padded 21-word shape and 640 for the 13-word one.

## Derivation, done before anything was simulated

Sources: `descriptor.h` (cost model, `vidcmd_plan_line()`),
`interpret.cpp::EngineWalker`, `vidcmd-spec.md` §4,
`griffin-video-datasheet.html` §6 PN1–PN12.

### D1 — framing

The program's authored slot sum is `64 + g + 64 + t + 64`, with
`g + t = SP_MIDDLE_PIXELS = 448`, hence exactly **640**.  Every playback record
plays at least `SP_POSE_SPAN = 8` slots — four times the two-slot fetch cadence
(`VIDCMD_SLOTS_PER_WORD`) — so no record can land behind its authored slot and
**occupancy == authored sum** (spec §4.3: "a record list must average two slots
per record").  The leading `SET(pix_base)` is the program's first word and the
line before it framed at exactly 640, so it is consumed in *this* line's blanking
at zero slots (§4.2: "Hblank SETs are therefore still free — zero slots — and are
guaranteed applied before the next line's pixel 0, in stream order").

*Measured:* 0 of 480 lines miss 640 slots, 0 stretch, 0 cadence hold slots,
0 overruns, 0 starvation holds, on every clean variant.

### D2 — the hblank path, in SYSCLK from the synchronized HBLANK edge

`edma3` splits a `wait_hblank` descriptor: the four-word fetch and
`HBLANK_RELEASE` happen when the walker *reaches* it, in the previous group, and
only arbitration + assert + payload + release happen at the edge.  PIX_PRELOAD is
`H_BLANK − PIXEL_FETCH_LEAD = 144` pixel clocks after the group start, i.e.
`floor(144·560/1007) = 80` SYSCLK.

| arrangement | first PIXELS word | PN7 margin | derived | measured |
|---|---|---|---|---|
| monolithic, VIDCMD first | `2+5+42+1+13+2` | 80 − 65 | **+15** | **+15** |
| pieces, PIXELS first | `2+5+2` | 80 − 9 | **+71** | **+71** |
| pieces, VIDCMD first (k=0) | `2+5+4+1+16+2+1+14+16+1+14+2+1+14+2+1+13+2` | 80 − 105 | **−25** | **−25** |

All three matched to the SYSCLK.

**Side finding: the Smurf hand derivation is conservative by seven words.**
`20 + 2W + 15 <= 78` charges the parked descriptor's 4-word fetch, its ASSERT and
its HBLANK_RELEASE to the hblank.  The engine pre-pays them in the previous
group, so the real path is `2 + 4 + 1 + 2W + 1 + 13 + 2 = 2W + 23 <= 80`, i.e.
**W <= 28**.  At W = 21 that is 65 SYSCLK and the measured +15 margin.  The
headroom is 7 words *in one descriptor*; it is not room for four more
descriptors, which cost 14 SYSCLK each.

### D3 — per-line bus cost

| | descriptors | payload words | group SYSCLK (derived = measured) |
|---|---|---|---|
| plain line, either form | 1 VIDCMD + 2 PIXELS | 1 + 40 | **130** |
| sprite line, monolithic | 1 VIDCMD + 2 PIXELS | 21 + 40 | **170** |
| sprite line, pieces | 5 VIDCMD + 2 PIXELS | 13 + 40 | **210** |

Line = 445 SYSCLK; per-line delivery budget = `VIDCMD_LINE_SYSCLK_BUDGET` = 294
SYSCLK = 147 words.  The pieces form costs **+40 SYSCLK** on a sprite line
(38% → 47% of the line) while moving **8 fewer payload words**: it trades bus
payload for descriptor turn-around.  Both forms are inside the budget.

### D4 — FIFO occupancy

Derived upper bound `(k+1) · 13` words when the whole lookahead window is sprite
lines.  Measured high-water: **21 (k=1), 34 (k=2), 60 (k=4), 112 (k=8)** — a law
of `13k + 8`, five words under the bound because the compositor has already
consumed the current line's first records by the time the k-th program lands.
Against a 256-word FIFO that is `(256 − 8)/13 = 19` sprite lines of lookahead
available, and no overflow at any k tested.  (The monolithic form at k = 0 sits
at 18.)

### D5 — negative controls

Each control must fail, and fail in its own way, or the deadline this case exists
to measure is not being measured.

| control | what it breaks | measured |
|---|---|---|
| `k0-vc` pieces, k=0, VIDCMD first | PN7: first PIXELS word 25 SYSCLK past PIX_PRELOAD | 64 late lines, 4 tiled PIXELS words, **678 wrong pixels** |
| `k0-px` pieces, k=0, PIXELS first | the line's own VIDCMD words arrive 109 SYSCLK after its pixel 0 | 960 late lines, 102 starvation holds, 958 overruns, **7,853 wrong pixels** |
| `k4-pre-rs` pieces, k=4, bootstrap before `/RS` | the pulse empties the FIFO | 5,048 starvation holds, 950 overruns, **9,948 wrong pixels** |

`k0-vc` is worth a note: at 1bpp the PIXELS FIFO accumulates surplus across
lines (high-water 38 of 40 words per line), so a 25-SYSCLK preload miss on every
sprite line only tiles 4 words — the picture is wrong in 678 pixels rather than
catastrophically.  At 80 words/line there would be no surplus to absorb it.  It
is still a PN7 violation on every sprite line.

`k4-pre-rs` is the sharpest lesson in the experiment: **the deposit-time margin
measurement declared it clean** (VIDCMD +1671, PIXELS +71, zero late lines,
identical to `k4`) because every word really did go on the bus on time.  The loss
is on the FIFO side.  Delivery accounting cannot see a `/RS` flush; PN5 has to be
satisfied by construction.

## The four questions

### 1. Does COMPOSITOR care *when* a word is deposited, as opposed to its order?

**No — with one absolute-time exception and one ordering corollary.**

Execution is positional.  `vidcmd-spec.md` §4.2 is explicit three times over:

> "Eagerness is **positional, not temporal**: a staged RUN waits for H_ACTIVE
> and, while it waits, blocks everything behind it."

> "Hblank SETs are therefore still **free — zero slots** — and are guaranteed
> applied before the next line's pixel 0, in stream order."

> "ENGINE descriptor waits shape bus traffic only; the FIFO decouples deposit
> time from effect time."

Each mechanism the question names is positional in the same way:

* **Eager SETs in blanking** — a SET at the head of line *y*'s program is
  consumed on the first blank clock of line *y*'s blanking, because line *y−1*'s
  program framed at exactly 640 slots and the first playback record behind the
  SET is a RUN that blocks until H_ACTIVE.  Exact-640 CUSHION framing is what
  makes *stream position* ≡ *raster position*; that equivalence is the whole
  reason lookahead is safe.
* **The palette write's cover rule** (§3.2, `descriptor.h` "THE PALETTE") is
  measured in slots **after the data SET's own slot**, i.e. from execution.  The
  three-word entry only has to be *consecutive in the stream*.
* **`SET pix_mode` / `pix_base` / `pixel_skip` per line** — PN7 requires them
  "already executed" by PIX_PRELOAD, 16 pixel clocks before the line.  A SET at
  the head of the line's program executes ~144 blank clocks before that.

The experiment probes this directly rather than taking it on trust: every sprite
line's cap carries `SET(pix_base, ·)` alternating between the two 1bpp pairs, so
the playfield colour under the sprite band alternates line by line.  If execution
were temporal, a word deposited 4 lines early would move that banding 4 lines.
Measured: **0 differing pixels** at k = 1, 2, 4, 8.

The exception is `/RS`.  TIMING pulses it once per frame at the top of the vsync
line and it empties both video FIFOs (PN5; `CompositorUnit::reset()` clears
`head_`, `count_` and `staged_word_`).  A word deposited before the pulse is not
early, it is gone.

The corollary, found while building this: **stream order is global.**  Line *y*'s
program cannot be early unless lines `< y` are too.  Lookahead is a uniform
offset applied to the whole frame, not a per-sprite optimisation.

### 2. With lookahead, what bounds the descriptors per line?

**The per-line delivery budget.**  It is not close.

```
per-line delivery budget  294 SYSCLK − 114 (40-word 1bpp PIXELS) − 26 (13 words)
                        = 154 SYSCLK, at 14 SYSCLK of turn-around each → 11 descriptors
the whole 445-SYSCLK line  the same way                              → 21 descriptors
the 256-word VIDCMD FIFO   19 lines of 13-word programs in flight    → bounds k, not descriptors
the hblank                 does not bind at all any more
```

The hblank stops binding precisely because the words are no longer for the
imminent line.  The only thing that must still fit before PIX_PRELOAD is the
PIXELS run's first word, which lands at 9 SYSCLK of the 80 available.  So the
measured configuration (5 pieces, 210 SYSCLK) has room for **six more piece
descriptors per line** inside the 66% budget, or sixteen more if the line is
allowed to spend its whole bus.

### 3. The frame's first sprite line, and after vblank

**Nothing special happens at the first sprite line.**  Line 200's pieces go in
group `200 − k`, an ordinary group that was already spending 130 SYSCLK on a
plain line; carrying a sprite program takes it to 210.  There is no "FIFO is
cold" problem at a sprite band, because the band is in the middle of a stream
that has been flowing since line 0.

**The cold start is the frame, not the sprite.**  Lines 0..k−1 have no group, so
their programs go in the vblank preamble.  The window is `/RS` (line
`V_SYNC_START` = 490) to line 0, i.e. lines 491..524 — **34 line times ≈ 15,100
SYSCLK**, thousands of descriptors' worth of otherwise idle bus.  What actually
bounds the pre-line-0 lookahead is the FIFO: 256 words, so 19 sprite-line
programs or 256 plain lines.  Any k a driver would plausibly want is available.

The suite's frame lock arms the list at line 480, ten lines *before* `/RS`, so
the preamble has to be pushed past the pulse by
`SP_PACERS_BEFORE_PREAMBLE = V_SYNC_START − FIRST_ARM_LINE + 1 = 11` of the 44
`wait_hblank` pacers that already walk the list to the top of frame.  That is the
whole cost of condition 3: reorder the existing pacer walk.  Getting it wrong is
the `k4-pre-rs` control.

### 4. Is a mixed scheme possible — pads removed entirely?

**Yes, and the measured configuration already is one.**  Two independent facts:

* **The pads never did framing work.**  A line frames at 640 on its playback
  records alone (D1).  The pads are target-7 SETs, decoded nowhere, consumed
  eagerly in the *next* line's blanking at zero slots — 8 words is 16 blank
  clocks of the 144 available.  Proof: `ref` reports 0 overruns, and the 13-word
  form is pixel-identical to the padded 21-word form.
* **The pads bought a fixed descriptor shape, and that is no longer worth
  anything.**  Once a line's descriptor *count* is per-line anyway (1 for a plain
  line, 5 for a sprite line), a fixed word count buys nothing.  Dropping them
  saves 8 words × 2 SYSCLK = 16 SYSCLK and 8 FIFO words per sprite line.

The measured list is mixed at both levels: plain lines are **one** descriptor
pointing at **one shared static word** — gap-table entry 640 *is*
`RUN(passthrough, 640)` — and sprite lines are five.

One thing moves, though.  The 640-slot closure has to be arranged in the static
data rather than at fill time: `tail = H_ACTIVE − 2·border − gap − pose_slots`.
Here `pose_slots` is a constant 64, so `tail = 448 − gap` and the tail word is the
same table indexed by a subtraction.  A pose whose spans are not uniform makes
`pose_slots` a per-pose constant — still one subtraction, still no copying, but
it has to be stored with the pose.

And the planner caveat: `vidcmd_plan_line()` reports 654 slots for the padded
shape because it has no H_ACTIVE fall.  Framing is planned over the semantic
prefix; a padded program cannot be closed by `close_exact()`-style arithmetic.

## What `apps/smurf` would have to change

* **Descriptor count.**  1 → 5 on a sprite line.  Over the whole frame:
  1,486 → 1,614 descriptors, 11,888 → 12,912 table bytes, against 32,768 per
  double-buffered half.  Fits with room to spare.
* **The 2,048-byte carve goes away entirely.**  The monolithic form needs
  32 × 64 = 2,048 bytes of scratch per frame, double-buffered because a display
  list's source RAM is frame-owned (PN4).  The pieces form needs **no scratch**:
  a 640-word (1,280-byte) `RUN(passthrough, n)` table, the pose rows (which are
  the art), two 2-word caps and one border word — all read-only, written once,
  **nothing to double-buffer and nothing to tear.**
* **The slot layout.**  The frozen 21-word shape is replaced by 13 semantic words
  with no padding.  Gap and tail are picked as a pair from one table
  (`tail_index = 448 − gap_index`); the pose row's total slot count becomes part
  of the pose's metadata.
* **Intra-group order inverts.**  The line's PIXELS descriptors go first and own
  the group's `wait_hblank`; the pieces follow.  The PIX_PRELOAD margin improves
  from +15 to +71 SYSCLK, which is the real prize — the current shape has 15
  SYSCLK of slack on the deadline PN7 calls out as unverified on hardware.
* **Bus.**  +40 SYSCLK on a sprite line (170 → 210 of 445), −8 payload words.
  Still inside the 294-SYSCLK / 147-word per-line budget.
* **CPU.**  Per sprite line the driver writes **three descriptor source-pointer
  words** (gap, pose, tail) instead of 21 VIDCMD words plus a program pointer.
  The cap and border descriptors never change after they are built.
* **The frame's shape.**  A k-line pipeline appears: the vblank preamble
  bootstraps lines 0..k−1 *after* `/RS` (behind 11 of the existing pacers), and
  the last k groups carry no program.  `k = 1` is enough and costs one line of
  bootstrap.
* **The new class of bug to watch for.**  The driver's "line y" and the list's
  group index now differ by k.  Off-by-k in the group index is silent: it shifts
  the whole sprite band vertically, which is exactly what `k4-pre-rs` looks like.

## Running it

```
cd super-engine && make check          # m11-static-pieces is near the end
```

Artifacts: `m11-static-pieces-ref.ppm` (monolithic reference) and
`m11-static-pieces-k4.ppm` (five pieces, four lines of lookahead).  They are
byte-identical by construction; the case asserts it.

Code: `author.h` / `author.cpp` (`StaticPiecesParams`,
`write_static_pieces_data()`, `author_static_pieces()`, the `sp_*` geometry) and
`main.cpp` (`run_static_pieces()`, `measure_sp_delivery()`,
`check_sp_geometry()`, the `m11-static-pieces` block).
