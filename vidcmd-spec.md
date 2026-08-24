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
      01 = held_fg, 10 = held_bg, 11 = RUN_COLOR (below; it displaced
      the invert-run candidate).

TILE  DROPPED.  `01` is a RESERVED one-slot no-op — see below.

RUN_COLOR { 2'b00, 2'b11, colour[2:0], ~count[8:0] }         1 word
      RUN src=11 spends its source encoding on a literal: colour is
      {R,G,B}, each bit replicated across its nibble, giving the eight
      saturated corners.  Never touches held_fg/held_bg, so independent
      objects on one line cannot clobber each other's colour.  The count
      field is 9 bits, so a span is at most 511 pixels.

SET   { 1'b1, target[2:0], value[11:0] }                     1 word
      target 0 = cmp_held_fg     4 = pix_ham_held
             1 = cmp_held_bg     5 = pix_mode
             2 = pix_pal_fg      6 = pix_pixel_skip
             3 = pix_pal_bg      7 = spare
```

Held colors persist until rewritten; `/RS` at vsync resets them to
fg = 0xFFF, bg = 0x000 and clears all machine state.

**TILE is dropped (user decision).**  compositor.v decodes the `01`
prefix as a record that consumes exactly one slot and changes nothing, so
a stray word costs a pixel instead of desynchronising the source
registers — and nothing emits it.  The two 16-bit mask shifters were the
biggest flip-flop lever available and they have been spent; sprites and
cursors are RUN / RUN_COLOR span lists instead, which cost words rather
than macrocells.  Retired encoding, for the record:
`{ 2'b01, ~skip[9:0], flags[3:0] }` plus a select mask word and an
opacity mask word, 3 words, MSB leftmost.  A real 3-word TILE arriving
now would have its two mask words interpreted as fresh commands.  If `01`
ever returns as a limited 2-word variant it gets whatever flip-flops are
left over; invert-run is not implemented either.

## 4. Execution model

### 4.1 During active video (H_ACTIVE)

**Playback is unchanged: one slot per clock, one slot per record.**  RUN
and RUN_COLOR consume their counts; a SET and the reserved no-op cost one
slot each.

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
  immediately at pop, in stream order, and the first RUN or RUN_COLOR
  popped parks in staging and waits for H_ACTIVE, releasing at pixel 0
  with the next record already banked behind it.
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
  stretch.
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

Registers (deltas vs. the superseded spec): held_fg/bg now 24 total,
RGB_OUT 12; line_word_cnt and drain-to-N deleted; +2 /EF sync; +1
ef_at_pop for the fetch engine (/RE's own level carries the parked
state, so there is no separate "parked" flip-flop); +12 set_pix_value.
The TILE mask shifters are gone.  **FIT SETTLED**: 112 of 128 macrocells
as built (down 4 from the value-bus baseline), 102 testbench checks
passing.  Handoff-edge loads are PT-dense: keep single-literal enables
(the PIXEL/HAM lesson).

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
4. ~~TILE flags~~ **CLOSED — TILE dropped** (§3).  `01` is a reserved
   one-slot no-op.  Absolute-X dies with it.
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
