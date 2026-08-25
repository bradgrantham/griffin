# PIXEL + COMPOSITOR / VIDCMD — Combined Design

Status: design record, pre-fit.  Captures the architecture converged on in
design discussion 2026-08-07/08.  **Supersedes compositor-overlay-spec.md**
(fixed-N framing, 8-bit color, zero-duration SET records are all replaced
here).

> **Addendum 2026-08-19 — cadence and conduit updated to compositor.v as
> built.**  Two things in this record are now history rather than design,
> and both are marked in place rather than quietly rewritten.
>
> * **Fetch cadence.**  The VIDCMD read port is a *registered* /RE driving
>   the IDT7200 pair directly — **two pixel clocks per word**, no shaping
>   gate.  The 74AC00 scheme this document's timing assumed is REJECTED on
>   the pinned worst-case table in `griffin.yml` (`interfaces:`, "VIDCMD
>   FIFO read port", 2026-08-18).  Playback is unchanged at one slot per
>   record; only delivery slowed.  §4 says what that costs.
> * **SET conduit.**  PIXEL no longer taps the VIDCMD Q bus.  The payload
>   crosses on a dedicated 12-bit registered bus (§5).
>
> The **K constants are unchanged** by both: pipeline depth is still 2
> clocks from H_ACTIVE to RGB_OUT, a SET is still visible in the pixel of
> the slot it occupies, and set_pix_commit still leads its own slot's
> RGB_OUT by one clock.  Normative source for all of it is
> `cpld/compositor/compositor.v` (the laws are in its header) and
> `cpld/compositor/compositor_tb.v` (102 self-checking traces).
> `super-engine/set-timing.v` is the ORIGINAL sketch of §5's retired Q-tap
> conduit and predates both changes; it is kept as a record of the
> shadow/pending scheme, not as a model of the chip.

> **Addendum 2026-08-24 — MASK is back on the `01` prefix, and the two
> COMPOSITOR colours are renamed.**  Both are implemented and committed in
> `cpld/compositor/compositor.v`.
>
> * **`01` is MASK, not a reserved no-op.**  Two words, sixteen pixels of
>   per-pixel overlay, no inline colour.  §3.1 is the encoding and §4.4 the
>   execution model; everything this document said about `01` being a
>   one-slot no-op is superseded.  TILE stays dropped — MASK is a much
>   smaller record that happens to reuse its prefix.
> * **`cmp_held_fg` / `cmp_held_bg` are `cmp_color1` / `cmp_color0`.**  A
>   dibit picks between them by NUMBER, so "fg/bg" stopped describing what
>   they are.  `griffin.yml`'s `VIDCMD_SET_CMP_HELD_FG` / `..._HELD_BG`
>   became `VIDCMD_SET_CMP_COLOR1` / `..._COLOR0`.  **The numbering and the
>   wire encoding did not move**, which means the standing oddity moved with
>   it: **SET target 0 writes colour 1 and target 1 writes colour 0.**
>   Renumbering the targets is a wire-contract change and is deliberately
>   still open (`griffin.yml` `issues:`).
> * **Unchanged:** every K constant, the 2-slot fetch cadence, the banked-pair
>   law, RUN / RUN_COLOR / SET.  MASK spends no new pins and no new counters
>   — it reuses `staged_word` as its shifter and the shared playback counter.

---

## 1. Architecture

```
ENGINE (descriptor DMA) ──► PIXELS FIFO ──► PIXEL ──► COMPOSITOR ──► 12-bit DAC
                       └──► VIDCMD FIFO ──────────────────┘
                                  PIXEL ◄── set_pix_value[11:0] ── COMPOSITOR
                     TIMING ──► PIXEL_CLK, LINE_START, H_ACTIVE, /RS
```

Two FIFOs decouple ENGINE's DMA from the video pipeline.  Neither PIXEL
nor COMPOSITOR has a CPU bus interface; all data and configuration arrive
in-band.  Everything runs in the shared 25.175 MHz PIXEL_CLK domain.

* **PIXELS is the data plane**: high rate, fixed length per line per mode,
  no semantics beyond pixel bits.  No in-band header — the old 4-byte
  palette/mode header is retired; those values arrive as VIDCMD SETs.
* **VIDCMD is the control plane**: variable length per line,
  position-synchronized instructions that composite over the picture and
  write registers in both chips.  It is the out-of-band register-write
  path: PIXEL needs no bus port and ENGINE needs no register-write DMA op.

Color is R4G4B4 (12-bit) throughout: PIXEL registers, the inter-chip RGB
bus, COMPOSITOR held colors, and the resistor DAC (three 4-bit ladders;
LSB ≈ 47 mV at VGA levels, 1% resistors adequate).

## 2. PIXELS stream

| mode | rate | words/line | meaning |
|---|---|---|---|
| 1bpp direct | 1 bit/clock | 40 | bit selects pal_fg / pal_bg |
| 2bpp micro-HAM | 2 bits/clock | 80 | dibit codes, below |

Micro-HAM codes are unchanged from the 1bpp design, just consumed at
2 bits/clock (so each op spans half as many pixels):

```
0_p     1 clk   held <- pal[p]           (p: 1 = fg, 0 = bg)
10_g_r  2 clk   held green <- g, held red  <- r   (1-bit, replicated to 4)
11_g_b  2 clk   held green <- g, held blue <- b
```

Line start: held <- pal_fg.  No cross-line state.  Full-precision colors
enter via VIDCMD SET (pal_fg, pal_bg, ham_held), not via HAM codes —
that is why the code space stays this small.

## 3. VIDCMD instruction set

All words 16 bits.  Counts stored complemented (`~count`): playback
counters load the complement and count up to the all-1s terminal.

```
RUN   { 2'b00, src[1:0], ~count[11:0] }                      1 word
      For count pixels: src 00 = passthrough (RGB_IN),
      01 = cmp_color1, 10 = cmp_color0, 11 = RUN_COLOR (below; it
      displaced the invert-run candidate).

MASK  { 2'b01, d1, d2, d3, d4, d5, d6, d7 }                  2 words
      { d8, d9, d10, d11, d12, d13, d14, d15 }
      Sixteen pixels of per-pixel overlay.  §3.1.

RUN_COLOR { 2'b00, 2'b11, colour[2:0], ~count[8:0] }         1 word
      RUN src=11 spends its source encoding on a literal: colour is
      {R,G,B}, each bit replicated across its nibble, giving the eight
      saturated corners.  Never touches cmp_color1/cmp_color0, so
      independent objects on one line cannot clobber each other's colour.
      The count field is 9 bits, so a span is at most 511 pixels.

SET   { 1'b1, target[2:0], value[11:0] }                     1 word
      target 0 = cmp_color1      4 = pix_ham_held
             1 = cmp_color0      5 = pix_mode
             2 = pix_pal_fg      6 = pix_pixel_skip
             3 = pix_pal_bg      7 = spare
```

Held colors persist until rewritten; `/RS` at vsync resets them to
`cmp_color1` = 0xFFF, `cmp_color0` = 0x000, abandons any mask playback,
and clears all machine state.

**Target 0 is colour 1 and target 1 is colour 0.**  Not a typo: the
numbers and the colour names disagree, the wire encoding is frozen by
`compositor.v`, `super-engine/descriptor.h`, the emulator, `apps/` and the
firmware, and renumbering them is a separate contract change that is still
open.  Nothing may quietly "fix" it in passing.

**TILE is still dropped (user decision); MASK reuses its prefix.**  The
two 16-bit TILE mask shifters were the biggest flip-flop lever available
and they were spent — MASK does not bring them back: it shifts the record
through `staged_word`, the buffer the fetch already had, and counts its
sixteen slots on the shared playback counter, so it costs two macrocells
plus one saved-source register.  Retired TILE encoding, for the record:
`{ 2'b01, ~skip[9:0], flags[3:0] }` plus a select mask word and an
opacity mask word, 3 words, MSB leftmost.  Absolute-X died with it, and
invert-run is not implemented either.

### 3.1 MASK — the two-word `01` record

```
word 1  { 2'b01, d1, d2, d3, d4, d5, d6, d7 }     d1 in bits [13:12], MSB-first
word 2  { d8, d9, d10, d11, d12, d13, d14, d15 }  d8 in bits [15:14]
pixel 0 has NO DIBIT: implicitly {1,0}, opaque cmp_color0.

dibit {opacity, select}:  00 passthrough (RGB_IN)   10 cmp_color0
                          11 cmp_color1            01 reserved -> 00
```

**There is no inline colour.**  All fourteen payload bits of the header
are dibits; recolouring a mask is an ordinary SET in front of it, which
costs what every other record costs.  The `01` prefix plus pixel 0's two
implicit opacity bits are the only bits the record does not spend on the
picture — sixteen pixels for two words, against the RUN-span lists that
cost a word per contiguous segment.

**The record is MODAL.**  A dibit overrides the source for its own pixel
only; whatever RUN was in force resumes, as if untouched, at pixel 16, and
RUN_COLOR's colour latch is not disturbed either.  The corollary is the
one that catches authors out: **dibit 00 is PASSTHROUGH — PIXEL's RGB_IN —
not "the span underneath".**  A mask laid over a RUN_COLOR background must
paint that background itself, out of `cmp_color0`.

**The data word is never decoded.**  It is captured with `have_staged`
LOW, so no bit pattern of it can reach the SET/RUN decode or the PIXEL
forwarding bus.  A stray `01` therefore paints sixteen junk pixels and
eats the word behind it as dibits — frame-bounded, cleared by `/RS`, and
the price of a two-word record in a stream with no framing.

## 4. Execution model

### 4.1 During active video (H_ACTIVE)

**Playback is one slot per clock.**  RUN and RUN_COLOR consume their
counts; a SET costs one slot; a MASK costs sixteen, its header's own slot
(pixel 0) included.

* **A SET occupies one slot and commits on the edge that ENDS it** —
  entry-edge commit — so its new value is visible in the pixel of its own
  slot, and a RUN emits its first pixel in the slot in which it is
  consumed.  (compositor_tb NORMATIVE_M0, POSITIONAL_EAGER.)
* Therefore: **a SET preceded by playback records totaling N slots takes
  effect starting at pixel N** (plus the constant per-target screen
  offset K, §6) — *provided the stream kept up, which is now a separate
  question; see below.*  Every boundary is reachable; `...passthrough,
  SET, passthrough...` renders as one unbroken passthrough whose state
  changes exactly on the boundary.

**Delivery is not one slot per record (2026-08-19).**  The fetch engine
is registered /RE, **two pixel clocks per word**, with a bank exactly two
deep — `staged_word` plus a word parked on the FIFO's Q while /RE is held
low.  Three rules, all evaluated from registers plus the current
H_ACTIVE:

* **fall** — /RE may go low at an edge only if it was high for the whole
  cycle ending at that edge and the guarded /EF shows data.  The 7200
  advances its read pointer on that falling edge and presents the word.
* **capture** — when /RE has been low for a full cycle the word is valid
  at the ending edge; if `staged_word` is free, *or is freed by that same
  edge's consume*, the word lands in `staged_word` and /RE registers high.
* **park** — otherwise /RE stays LOW and Q holds the word until a later
  edge frees `staged_word`.  /RE's own level carries the parked state, so
  a parked word cannot be overwritten.

Two consequences the list builder must plan around:

* **Sustained cadence: 2 slots per word.**  Fall at edge k, capture at
  k+1, fall again at k+2.  A record that executes in one slot therefore
  paints its own slot plus one HOLD slot behind it — **one-pixel spans
  render two pixels wide** — and a list has to *average two slots per
  record* to land where it was authored.
* **BANKED-PAIR LAW.**  When a record executes with a second record
  parked on Q, the park moves into `staged_word` on that very edge, so
  the two execute on **consecutive slots**.  Every RUN long enough to
  cover a fetch builds such a pair, so this is the common case at a
  line's first records, not a corner.

So *"consecutive SETs land on consecutive pixels"* — the old blanket
claim — now holds **only for a banked pair**.  Two SETs behind a RUN of
four or more still commit on adjacent pixels (that is what the
mid-line-split case relies on, and it is why the pair law exists); a
**third** SET in the same burst lands two pixels after the second, not
one.  compositor_tb measures the burst at slots 1, 3, 5.

### 4.2 Outside active video

* Playback idles — no slots, no counting.  Fetch continues at the same
  2-clock cadence (blanking does not exempt it): a staged SET executes
  immediately at pop, in stream order, and the first RUN, RUN_COLOR **or
  MASK HEADER** popped parks in staging and waits for H_ACTIVE, releasing
  at pixel 0 with the next record already banked behind it.  **A mask
  header is playback-class, not setup**: its own slot is the record's
  pixel 0, so it paints and therefore waits.  A mask banked in blanking
  plays its pixel 0 in slot 0.
* Hblank SETs are therefore still **free — zero slots** — and are
  guaranteed applied before the next line's pixel 0, in stream order.
  What changed on 2026-08-19 is only their rate: a run of eager SETs
  executes at one per two clocks (a pair when one is parked), which is
  invisible against a 160-clock HBLANK and is exactly what a starved list
  runs out of.  Their exact clock depends on ENGINE's deposit timing —
  the emulator asserts the weak property (before line start) for blank
  SETs and the strong pixel-exact property for active SETs.
* Eagerness is **positional, not temporal**: a staged RUN waits for
  H_ACTIVE and, while it waits, blocks everything behind it.  An overrun
  therefore steals the next line's eagerness.
* No "wait" bits exist or are needed: RUN/RUN_COLOR are the visible
  timeline and the staged-record release is the only beam
  synchronization.  ENGINE descriptor waits shape bus traffic only; the
  FIFO decouples deposit time from effect time.

### 4.3 Line framing and errors

* **Each line must OCCUPY exactly 640 slots** under the cushion
  discipline.  Occupancy is the authored slot sum *plus the HOLD slots
  the 2-clock fetch spends between records*, so since 2026-08-19 this is
  no longer a sum the builder can do in its head.  Minimum program for an
  untouched line: one word (RUN passthrough 640) — one record can never
  stretch.  A MASK contributes 16 slots and a RUN-to-mask or mask-to-mask
  boundary contributes none, so a line of chained masks IS a sum again —
  forty records of sixteen is exactly 640 — and only the SET seams (§4.4)
  have to be priced.
* On starvation (FIFO empty mid-line) playback holds the current output
  mode.  With a VBLANK priming cushion the FIFO is normally never empty,
  so *starvation* hold is a degradation behavior; *cadence* hold is
  structural and unavoidable.  The two are counted separately in the
  super-engine model and only the first is a builder bug.
* Framing violations desync at worst until vsync: **errors are
  frame-bounded**, recovered by `/RS`.  The builder and emulator enforce
  the invariants; the silicon just runs.
* **Prefetch invariant, cadence-aware form (builder-enforced).**  The
  1-word-per-clock design retired this rule; the registered-/RE fetch
  brings it back.  It is no longer a closed form — whether a given record
  is half of a banked pair depends on the whole prefix — so the builder
  runs a deterministic **simulation of the fetch engine** over each
  line's record list and sizes that line's trailing filler against the
  slot the simulation actually delivers it on
  (`super-engine/descriptor.h`, `vidcmd_plan_line()`).  The rule of thumb
  behind it: **a record list must average two slots per record**, with a
  **bank credit of two words** from HBLANK — one staged, one parked on Q
  — as the line's entire free head start.  Anything denser is not late,
  it is impossible: one-pixel spans need a four-pixel pitch to land where
  they were authored.

### 4.4 MASK execution, and the seams it costs

The record is sixteen slots, and two edges inside it hand the shifter's
buffer back to the fetch.  Both are keyed off the playback count, both are
gated by H_ACTIVE, and between them they are the whole reason the form
works:

* **pixel 8** — the header's last dibit (d7) was read on the previous
  edge, so the header is spent: **the data word is taken out of the park
  here and d8 is read straight off Q.**  Playback does not pause; pixel 8
  lands in the slot after pixel 7.  If the data word has not arrived the
  record **STALLS whole** — no count, no shift, no source change — and
  pixel 7's colour stretches until it does, exactly like every other
  starvation here.
* **pixel 15** — d15 was read on this edge, so the shifter is dead
  afterwards: **the NEXT RECORD is captured here, one slot early.**

**Gapless chaining falls out of the pixel-15 edge.**  Mask B's header is
banked on Q while A plays pixels 8..15, moves into `staged_word` on A's
pixel-15 edge, and is staged with a terminal count during A's last pixel,
so the ordinary consume rule fires it on the very next edge.  A cursor or
a line of text of any width is a plain run of records.

**Seam constants.**  Slots between a record's last painted pixel and the
next mask's pixel 0 — 0 means adjacent pixels.  The first three are
measured by `compositor_tb.v`; the last two are derived from the same laws
and measured by the super-engine model's clean-room traces.

| sequence | slots | why |
|---|---|---|
| RUN → MASK | 0 | the header paints pixel 0 in its own slot |
| MASK → MASK | 0 | the pixel-15 edge captures the next header |
| MASK → SET → MASK | 2 | one record at the ordinary 2-slot cadence |
| MASK → SET,SET → MASK | 4 | the mask holds `staged_word`, so only ONE word parks on Q — the two SETs **cannot** be a banked pair, and each costs its own slot plus a HOLD |
| RUN → SET,SET → MASK | 3 | behind a RUN the pair law does apply, but the park held /RE low, so /RE can only rise on the edge that consumes SET 1 and fall on the edge after: one HOLD behind the pair |

The last row is not a mask rule — it is the shape `compositor_tb`'s
PAIR_LOCAL already pins (RUN(4), SETs on slots 4 and 5, HOLD on 6, next
record on 7).  It is listed here because it is the difference between
recolouring **in the background run** (3) and recolouring **between two
mask groups** (4), and an author who prices the wrong one gets art that is
one pixel out.

**Authoring rules**, all of them consequences rather than conventions:

* **Leading transparency is a RUN, not dibits.**  Pixel 0 is implicitly
  OPAQUE, so a sprite whose left edge is transparent does not start its
  record at its bounding box: it starts at the first opaque pixel, behind
  a passthrough RUN.  That costs nothing — RUN → MASK is zero slots.
* **Whatever pixel falls on a record boundary is `cmp_color0`.**  Art laid
  out in 16-pixel cells therefore wants a blank column at each cell's left
  edge.  A 5x7 font in an 8-pixel cell with a blank column 0 puts **two
  glyphs in one record**; the same font scaled x3 into a 16-pixel cell
  puts **one big glyph in one record**.  A big font that does not fit 15
  pixels needs two records per glyph and has its middle column forced to
  `cmp_color0`.
* **Recolour between groups, not inside a run of them**, and price the
  seam into the geometry.
* **A mask over a RUN_COLOR must repaint that background out of
  `cmp_color0`**, because dibit 00 is passthrough, not the span below.
* A mask caught by the H_ACTIVE fall **freezes**, it is not abandoned: the
  pixels that are left play at the start of the next line — wrong x,
  self-healing — and the parked data word is not taken during blanking.
  `/RS` abandons all of it.

### 4.5 The pure-VIDCMD screen mode

A display list can author **no PIXELS descriptors at all**: MASK records
for the art, RUNs for the background, SETs for the colour.  The display
list *is* the framebuffer, and there is no bitmap, no scroll register and
no pixel bandwidth.

**Budget.**  A line's VIDCMD words are delivered at
`ENGINE_CYCLES_PER_WORD` = 2 SYSCLK each.  The suite's cases sit at about
**66% of a 445-SYSCLK line**, i.e. **147 words/line**, which is the figure
`super-engine/descriptor.h` pins as `VIDCMD_WORDS_PER_LINE_CAP` and the
two screen cases are held to.  Against that:

| shape | words/line | slots/line | verdict |
|---|---|---|---|
| 80-column text, every group a record + 2 per-line SETs | 82 | 640 | fits, with room to spare |
| large text, one SET pair per menu item | ≤ 30 | 640 | fits easily |
| **every group recoloured** (SET pair + record per group) | **160** | **800** | over the 147-word cap **and** over the 640 slots — impossible, not merely late |

The last row is the rule of thumb: **recolouring is what runs out, not
records.**  Forty chained masks fill a line exactly; forty *recoloured*
masks do not fit a line at all.

**Three colours per record, for free.**  Inside a mask a pixel is
`cmp_color0`, `cmp_color1`, or passthrough.  With the PIXELS FIFO never
written, PIXEL re-delivers its reset word — all zeroes, which in 1bpp
direct mode selects `pix_pal_bg` — so passthrough resolves to `pal_bg`, a
register a SET can write.  Dibit 00 then behaves as a **third settable
colour**, available inside a group with no SET between groups.

> **This last paragraph is MODEL behaviour and is NOT yet verified against
> hardware.**  `super-engine/render.cpp` follows `pixel.v`'s documented
> tiling (no empty flag; a 7200 holds Q while empty, so an unfed stream
> re-delivers its last word), but what a 7200 pair presents on Q after
> `/RS` with no write at all, and what `pixel.v`'s shifter holds before its
> first fetch, are both open.  Settle those two before designing art around
> the third colour.

## 5. Cross-chip mechanism (the SET conduit)

**As built (decided 2026-08-13, `griffin.yml` `interfaces:` "COMPOSITOR
-> PIXEL SET register path"): a dedicated 12-bit registered value bus.**

* `set_pix_value[11:0]` — twelve point-to-point traces COMPOSITOR ->
  PIXEL carrying the staged word's payload, alongside the existing
  `set_pix_valid` / `set_pix_target[2:0]` / `set_pix_commit`.  All four
  are registered off the same staged word, so they are one pipeline
  stage.  PIXEL applies the bus **directly at the commit pulse** — no
  shadow, no pending, no capture register.
* **PIXEL no longer taps VIDCMD Q at all.**  Its twelve former Q taps
  repointed to this bus, which takes twelve loads off the shared FIFO
  data bus.
* The bus **must** be registered, and that is correctness rather than
  fitter preference: a combinational tap of `staged_word[11:0]` would be
  one cycle ahead of its own valid window.
* Consecutive PIXEL-target SETs still coincide: across a banked pair,
  SET N's commit lands on the same edge that registers SET N+1's value,
  so PIXEL's live registers must sit in one clocked block with
  non-blocking assignments — **apply-old, capture-new**.
* A SET waiting behind a long RUN simply holds the bus for as many cycles
  as it waits, which is harmless.

**Retired: the Q-tap shadow conduit.**  The original scheme had PIXEL
latch Q[11:0] on every observed /RE into a *shadow*, promote it to
*pending* when COMPOSITOR's registered PIX_TVALID/PIX_TSEL said the
previous pop was PIXEL's, and load pending on PIX_COMMIT.  It never
worked: at one word per clock Q had moved on by one or two words before
the capture.  The registered-/RE fetch then made it impossible outright —
Q is high-Z between reads and holds an unrelated word during them.
`super-engine/set-timing.v` is that scheme's sketch and is kept only as a
record of it.

Every cross-chip signal is FF-driven and FF-sampled; there are no
combinational paths through package boundaries.  The one async input,
/EF (deassert edge caused by ENGINE-domain writes), gets an
asymmetrically guarded 2-FF synchronizer in COMPOSITOR: the synchronizer
governs the asynchronous RISE, the raw flag governs the FALL caused by
COMPOSITOR's own clock-aligned read.

## 6. Timing constants

Pipeline depth (PIXEL register stage, COMPOSITOR output register, the
one-cycle registered set_pix_commit) gives each SET target a small
constant screen offset **K**.  K is *measured*, not derived, and
`cpld/compositor/compositor_tb.v` is now the thing that measures it —
it prints the constants and asserts them, the emulator and the
super-engine model assert the same values, and the list builder folds
them in.  **The 2026-08-19 cadence rework did not move any of them:**

| constant | value |
|---|---|
| H_ACTIVE rise -> matching RGB_OUT | 2 clocks |
| SET visible at | its own slot (offset 0) |
| set_pix_commit -> its own slot's RGB_OUT | 1 clock (commit leads) |
| PIXEL-target SET cost | same as any record, no fetch stall |

Semantics stay pixel-exact; K never varies at runtime.  What the rework
*did* change is the FETCH constants, which are not K: 2 slots per word
sustained, 1 slot of gap across a banked pair.

**Electrical budget.**  The worst-case table is PINNED in `griffin.yml`
(`interfaces:` "VIDCMD FIFO read port", 2026-08-18) and must not be
re-derived here; it is quoted in `compositor.v`'s header.  Headline
numbers at T = 39.72 ns with IDT7200L15 + ATF1508AS-10: data path
tCO 8.0 + tA 15 + ~2 flight = 25.0 vs T - tSU = 32.7, **+7.7 ns**; /EF
the same +7.7; pulse +24.7, recovery +29.7, cycle +54.4.  The ~35 ns
"pre-existing, unchanged" figure this table used to carry was the thing
that failed review — it assumed a half-period /RE and a shaping gate, and
the gate scheme is rejected structurally (a zero-delay gate still misses
every capture edge by 8-15 ns in every phasing).  Every edge in
COMPOSITOR is a GCLK rising edge, so nothing depends on the oscillator's
duty cycle.

Board notes: series-terminate every /RE and /W at the driver end
(promoted from the rev-1 ringing bodge to design); PIXEL_CLK skew between
chips < ~2-3 ns (same net, matched routing).  PIXEL no longer senses /RE.

## 7. Chip summaries

### PIXEL

Drains PIXELS at 1 or 2 bits/clock; resolves each pixel to R4G4B4 from
its register file; outputs one color/clock to COMPOSITOR.  Decodes no
instructions.

Registers: pal_fg, pal_bg, ham_held (12 each), mode, pixel_skip, HAM
decode FSM, stream shifter.  (The shadow and pending registers went with
the Q-tap conduit — the value bus is applied straight at the commit.)

Pins: PIXELS Q[15:0] in, RGB[11:0] out, **set_pix_value[11:0] in**,
set_pix_target[2:0] + set_pix_valid + set_pix_commit in, clocks/timing.
No VIDCMD Q tap and no /RE sense.  ≈ 48 of 64.

### COMPOSITOR

Executes the VIDCMD program in lockstep with scanout; drives the DAC.

Registers (deltas vs. the superseded spec): cmp_color1/cmp_color0 now 24
total, RGB_OUT 12; line_word_cnt and drain-to-N deleted; +2 /EF sync; +1
ef_at_pop for the fetch engine (/RE's own level carries the parked
state, so there is no separate "parked" flip-flop); +12 set_pix_value.
The TILE mask shifters are still gone, and MASK did not bring them back:
+1 `mask_active` and +2 `sav_src`, with `staged_word` doubling as the
dibit shifter and the shared playback counter doing the sixteen slots.
**FIT AS BUILT (2026-08-24)**: 121/128 logic cells (94%), 95 flip-flops,
124/128 nodes+FB, 370 product terms, 64/64 I/O.  The obvious way to write
the mask — an override in front of the colour mux — costs three product
terms on each of the twelve already-wide RGB_OUT macrocells and does NOT
fit; driving the dibit through `cur_src` and restoring it does, and is
externally identical.  Handoff-edge loads are PT-dense: keep
single-literal enables (the PIXEL/HAM lesson).

Pins: VIDCMD Q[15:0] in + /RE out + /EF in, RGB_IN[11:0], RGB_OUT[11:0],
**set_pix_value[11:0]** + set_pix_target/valid/commit out, timing in.
Pin-closed: 63/64 I/O + 3/4 dedicated inputs, one spare of each kind,
accepted because the interface is architecturally complete.

## 8. Signal table

| signals | path | function |
|---|---|---|
| RGB[11:0] | PIXEL -> COMPOSITOR | composited input, one color/clock |
| Q[15:0] | VIDCMD FIFO -> COMPOSITOR | instruction word (**no PIXEL tap**; high-Z between reads) |
| /RE | COMPOSITOR -> FIFO | registered, one clock low per word, 2 clocks per word sustained |
| /EF | FIFO -> COMPOSITOR | empty flag (2-FF sync'd rise, raw fall) |
| set_pix_value[11:0] | COMPOSITOR -> PIXEL | the staged word's payload, registered |
| set_pix_target[2:0], set_pix_valid | COMPOSITOR -> PIXEL | "the bus is yours, register N" |
| set_pix_commit | COMPOSITOR -> PIXEL | apply the bus now (leads its slot's pixel by 1 clock) |
| RGB_OUT[11:0] | COMPOSITOR -> DAC | final color |
| PIXEL_CLK, LINE_START, H_ACTIVE, /RS | TIMING -> all | clock, epoch, blanking gate, per-frame reset |

## 9. What 12-bit color costs elsewhere (summary)

* Zero memory bandwidth: pixel streams stay 1/2 bpp indexed; literals
  ride SETs and HAM palette entries.
* ENGINE: no change (word streamer).  Display-list builder writes wider
  literals; VBLANK preamble re-establishes held colors and palettes.
* Board: 12 DAC pins + three 4-bit ladders; PIXEL->COMPOSITOR RGB bus is
  12 traces, and since 2026-08-13 there are 12 more the other way for
  set_pix_value; new-board items alongside the COMPOSITOR/PIXEL chips.
* Emulator/tools: 4-bit channel expansion (c*17), SET/target decode,
  2bpp HAM, r4g4b4 quantizer; super-engine host suite re-run (word
  counts unchanged).

## 10. Open items

Kept with their original numbering; status stamped rather than deleted.

1. ~~COMPOSITOR fit experiment~~ **CLOSED 2026-08-19**: 112/128
   macrocells as built, no counter-shrink or skip-merge lever needed.
2. ~~PIXEL fit~~ **CLOSED**: two palette entries, and shadow/pending are
   no longer adds — the value bus is applied at the commit (§5).
3. ~~RUN src=11~~ **CLOSED**: RUN_COLOR took the encoding; invert-run is
   the natural trade if the fit ever needs the room back.
4. ~~TILE flags~~ **CLOSED — TILE dropped** (§3), and **REOPENED AND
   CLOSED AGAIN 2026-08-24**: the `01` prefix now carries MASK (§3.1,
   §4.4), a two-word sixteen-pixel overlay with no inline colour and no
   new shifters.  Absolute-X stayed dead with TILE.
7. **NEW, OPEN.** SET target 0 writes `cmp_color1` and target 1 writes
   `cmp_color0`.  The 2026-08-24 rename fixed the NAMES; whether to swap
   the target NUMBERS so they match the colour numbers is a wire-contract
   change across `compositor.v`, `super-engine/descriptor.h`, the
   emulator, `apps/` and the firmware, and is deliberately not bundled
   with the rename.
8. **NEW, OPEN, and it gates a whole authoring technique.** §4.5's third
   colour — passthrough resolving to `pix_pal_bg` when the PIXELS FIFO is
   never written — is MODEL behaviour only.  Confirm against `pixel.v`
   what the shifter holds before its first fetch, and against the
   IDT7200 datasheet what Q presents after `/RS` with no write, before
   any art depends on it.
5. ~~Pin down K constants from real RTL~~ **CLOSED**: measured by
   `compositor_tb.v`, propagated to the emulator and the super-engine
   model; unchanged by the 2026-08-19 cadence rework (§6).
6. **STILL OPEN, and reshaped by the cadence.**  VIDCMD FIFO depth /
   priming-cushion sizing vs. the ENGINE descriptor schedule.  Depth is
   no longer the interesting variable on its own: 256 words is 511 slots
   of one-slot records, so a dense line runs out of *cadence* before it
   runs out of FIFO, and the useful budget is words-per-line against both
   the bus rate (~1 word per 3.6 pixel clocks) and the fetch rate (1 word
   per 2 pixel clocks).
