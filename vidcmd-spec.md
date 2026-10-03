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

> **Addendum 2026-09-20 — PIXEL is INDEXED, the palette is a RAM chip, and
> TIMING is inside PIXEL.**  Implemented in `cpld/pixel/pixel.v` and measured by
> `cpld/pixel/{pixel_tb.v, timing_tb.v, clut_pair_tb.v}`; those three benches
> are the executable spec and win any disagreement with this document.  §2, §3's
> SET table, §3.2, §5, §6, §7 and §8 are **rewritten**, not annotated.
>
> * **Every playfield pixel is a four-bit palette index.**  PIXEL holds no
>   colour register at all: `pal_fg`, `pal_bg`, `ham_held`, micro-HAM, the
>   2bpp-indexed fixed four colours and the 12-bit `RGB_OUT` bus out of PIXEL
>   are **retired**.  The colour comes from an external 16 x 12 palette RAM
>   (§3.2) whose outputs are COMPOSITOR's `RGB_IN`.
> * **Four depths, and the mode register is an ENUMERATION again**: 1bpp, 2bpp,
>   4bpp, attribute, plus the half-rate flag.  The 2026-08-26 "mode is a bit
>   field" addendum is gone with the modes it described.
> * **The PIXELS FIFO is read as one ganged 16-bit port** with a single `/RE`;
>   the byte-interleaved shared-Q pair and its two strobes are retired.
> * **TIMING is not a chip.**  `timing.v`'s Timing module is instantiated inside
>   `pixel.v`, so PIX_CONSUME / PIX_PRELOAD / PIX_LAST are internal nets and
>   every other raster output is a PIXEL pin.
> * **COMPOSITOR is untouched** — RTL, pins and every VIDCMD word format,
>   including the SET encoding.  Only the meaning of targets 2, 3 and 4 moved.
> * **Unchanged:** every K constant (the palette RAM is combinational and costs
>   no clock), the 2-slot VIDCMD cadence, the banked-pair law, RUN / RUN_COLOR /
>   MASK / SET, and `PIXEL_OUT_LEAD` = 1.

> **Addendum 2026-10-03 — RUN_COLOR is gone; LITERAL, HOLD and STREAM RUN
> replace it, and the COMPOSITOR pinout moved.**  Implemented in
> `cpld/compositor/compositor.v` and measured by `compositor_tb.v`
> (`make compositor-sim`, `make compositor-mutants`) and `clut_pair_tb.v`
> case S6s; those benches win any disagreement with this document.  §3, §3.3,
> §4.1–§4.3 and §6 are **rewritten**; older addenda above that say "RUN /
> RUN_COLOR" are history.
>
> * **LITERAL** `{00, 11, colour[11:0]}` takes RUN_COLOR's prefix: one pixel
>   of any R4G4B4 colour.  It holds the on-chip buffer for its slot, so it
>   **always paints two slots** (its own plus one KEEP) and whatever follows
>   it executes two slots later, banked pair or not — one 320-density pixel.
> * **cur_src's fourth value is KEEP**: RGB_OUT holds the last colour shown.
>   **HOLD(N)** is a RUN with count bit 11 low: KEEP for N slots.
> * **STREAM RUN** is a RUN with count bits 11:10 = `10`: SETs staged behind
>   it execute while it counts, so a run's geometry is fixed while its
>   colour changes mid-span — and a late SET recolours late without moving
>   anything.
> * **The run counter is ten bits**; bits 11:10 are the record kind.  Every
>   old RUN word with a count of 1..1023 is valid bit for bit.
> * **Pinout**: RGB_OUT 3/0/1/2 swapped with VIDCMD_Q 10/11/8/13 (eight pins)
>   so that no logic block carries more than four RGB_OUT bits; the block is
>   PROPOSED rev-2, NOT FROZEN.  Fit 118/128 LC.
> * **Unchanged:** every K constant, the 2-slot fetch cadence, the banked-pair
>   law (except that a literal cannot head a pair), MASK, SET and the palette
>   rules.  Lost: the one-word span in the eight corner colours (now LITERAL +
>   HOLD, two words, any colour).
>
> Proposal and fit ladder: `vidcmd-literal-hold-proposal.md` (ADOPTED as
> form A + HOLD + streaming), `cpld/experiments/literal/RESULTS.md`.

---

## 1. Architecture

```
ENGINE (descriptor DMA) ──► PIXELS FIFO ──► PIXEL ─IDX[3:0]─► PALETTE RAM
                       └──► VIDCMD FIFO ──► COMPOSITOR ◄─RGB[11:0]─┘
                                            COMPOSITOR ──► 12-bit DAC
        COMPOSITOR ─set_pix_value[11:0]─► the RAM's data inputs; [3:0] ─► PIXEL
        COMPOSITOR ─set_pix_target/valid/commit─► PIXEL
        PIXEL ─nCLUT_WE─► the RAM's /WE   (its /CS is tied low)
        PIXEL ──► PIXEL_CLK domain's raster: H_ACTIVE, /RS, syncs, HBLANK, ticks
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

Color is R4G4B4 (12-bit) throughout: the palette RAM's entries, the RAM-to-
COMPOSITOR RGB bus, COMPOSITOR held colors, and the resistor DAC (three 4-bit
ladders; LSB ≈ 47 mV at VGA levels, 1% resistors adequate).  **PIXEL is the one
chip in the chain that never sees a colour**: it emits a four-bit index and, for
a palette write, an address and a write pulse.

The raster generator is inside PIXEL, so PIXEL_CLK, H_ACTIVE and /RS reach
COMPOSITOR and the FIFOs from there; there is no TIMING chip.

## 2. PIXELS stream

**Every playfield pixel in every mode is a four-bit palette index** (§3.2).
PIXEL owns no colour, and COMPOSITOR's own sources — `cmp_color1`,
`cmp_color0`, LITERAL, KEEP, MASK — are direct 12-bit values that never touch
the palette.

The two IDT7200s are read as **one ganged 16-bit port with one registered
`/RE`**, low for exactly one clock per word and never low on two consecutive
clocks.  Words are consumed **MSB first, bit 15 down**, so every record written
for the retired byte-interleaved port renders bit for bit the same (the byte it
read first is `PIXELS_Q[15:8]`).  There is no empty flag and no half-full
pacing: a 7200 ignores a read while empty and holds Q, so **an underrun tiles
the last word** — a short fill is a bandwidth compressor, not an error.

`SET pix_mode`: `mode[1:0]` is a **depth enumeration**, `mode[2]` is half rate
(each consumed group held two pixel clocks, 320 groups across the same
640-clock window), `mode[3]` is **reserved for the COMPOSITOR bypass and must
be written 0**.

| depth | `mode[1:0]` | index | words/line full | half |
|---|---|---|---|---|
| 1bpp | `00` | `{pix_base[3:1], bit}` | 40 | 20 |
| 2bpp | `01` | `{pix_base[3:2], dibit}` | 80 | 40 |
| 4bpp | `10` | the nibble (`pix_base` unused) | **UNDEFINED** | 80 |
| attribute | `11` | `bit ? fg : bg` | 80 | 40 |

**4bpp at full rate is undefined and must not be emitted**: four clocks per
word puts two fetch triggers inside the eight-clock end-of-line window, which
the cutoff cannot concede, so the line would end short of its record and every
following line of the frame would be out of phase.  160 words/line is over the
line's bus budget in any case.

**The attribute word** is `{fg[3:0], bg[3:0], pattern[7:0]}` — one word per
eight-group cell, `PIXELS_Q[15:8]` the colour byte and the pattern MSB first.
With a TMS9918 palette loaded it is the Graphics II colour byte followed by the
pattern byte.  At full rate it is an 80-column cell-coloured text/tile mode; at
half rate a 320-wide tile mode.  Attribute mode takes **no** `pixel_skip`.

**Mode changes belong in blanking.**  A line's descriptors supply that line's
word count, so the mode may differ line to line; a mid-line change is undefined
and unspecified.

### The fetch cadence (`pixel_tb.v`, measured)

With W stream bits per group and R clocks per group (R = 2 at half rate), a
16-bit word lasts 16R/W consumption clocks and the fetch trigger sits at stream
position T:

| mode | T full | T half | clocks/word full | half |
|---|---|---|---|---|
| 1bpp | 14 | 15 | 16 | 32 |
| 2bpp | 12 | 14 | 8 | 16 |
| 4bpp | 8 | 12 | 4 | 8 |
| attribute | 6 | 7 | 8 | 16 |

(Attribute mode's word carries eight pattern bits, so its cell is eight groups
and its trigger compares the low three bits of the stream position only.)  The
**floor over every defined mode and skip is 8 clocks between reads** — 318 ns
at 25.175 MHz, against the 2 clocks COMPOSITOR already runs the same part at,
so no tRR/tRC arithmetic is needed here.

### pixel_skip — a start position inside the line's first word

`pixel_skip` is four bits counting **stream bits**: skip *s* means pixel 0 is
stream bit *s* of the line's first word.  There is no whole-word discard — a
four-bit skip cannot leave the first word — and coarser scrolling moves the
descriptor's source pointer by whole words.

* **Depth clamp**, because a W-bit group can only shift by multiples of W:
  1bpp honours all four bits, 2bpp ignores bit 0, 4bpp ignores bits [1:0],
  attribute forces 0.
* **Top-of-range clamp**, two independent bounds and the smaller bites: the
  trigger's lead T (**full rate only**) and the preload's own lead, 14 at both
  rates.  Net caps: **14** (1bpp either rate), **12** (2bpp full), **14** (2bpp
  half), **12** (4bpp half).  Above the cap the value is clamped, not honoured —
  a recorded limitation, not a fault.
* The clamp is applied **at consumption** (the line's preload), so SET ordering
  cannot smuggle a stale bit in, and **skip is sampled at that preload**: a
  mid-line `SET(pixel_skip)` affects the next line only, its word count
  included.

### Words per line — the rule the display-list author must satisfy

```
words per line = record + (pixel_skip != 0 ? 1 : 0)

record = 40 (1bpp full)   20 (1bpp half)   80 (2bpp full)   40 (2bpp half)
         80 (4bpp half)   80 (attr full)   40 (attr half)
```

A line entered at bit *s* > 0 gets only 16 − *s* bits out of its first word, so
it needs **one more word** than an unskipped line, and **its tail shows real
stream data** rather than zeros — which is what a horizontal scroll wanted.
The author must supply that word; with the clamp in force exactly one extra word
is ever read, never two.  `pixel_tb.v` asserts the whole 65-combination
mode/skip matrix at the real 640-clock line width, two consecutive lines each.

## 3. VIDCMD instruction set

All words 16 bits.  Counts stored complemented (`~count`) in a TEN-bit
field: the playback counter loads the complement and counts up to the
all-1s terminal.  Bits 11:10 of a run-class word are its KIND, never count.

```
RUN        { 2'b00, src[1:0], 2'b11, ~count[9:0] }           1 word
      For count pixels: src 00 = passthrough (RGB_IN),
      01 = cmp_color1, 10 = cmp_color0.  count 1..1023; 0 means 1024.

STREAM RUN { 2'b00, src[1:0], 2'b10, ~count[9:0] }           1 word
      A RUN during which staged SETs execute without waiting for its
      terminal.  §3.3.

HOLD       { 2'b00, src[1:0], 1'b0, x, ~count[9:0] }         1 word
      KEEP for count pixels: RGB_OUT repeats the last colour shown.
      src is ignored (but must not be 11, which is LITERAL).  §3.3.

LITERAL    { 2'b00, 2'b11, colour[11:0] }                    1 word
      One pixel of any R4G4B4 colour, then KEEP.  Always two slots wide.
      Never touches cmp_color1/cmp_color0.  §3.3.

MASK  { 2'b01, d1, d2, d3, d4, d5, d6, d7 }                  2 words
      { d8, d9, d10, d11, d12, d13, d14, d15 }
      Sixteen pixels of per-pixel overlay.  §3.1.

SET   { 1'b1, target[2:0], value[11:0] }                     1 word
      target 0 = cmp_color1      4 = pix_base[3:0]
             1 = cmp_color0      5 = pix_mode[2:0] (+ reserved bit 3)
             2 = pix_clut_index[3:0]
             3 = pix_clut_data[11:0]  -- written to the palette (§3.2)
             6 = pix_pixel_skip[3:0]
             7 = ignored by both chips: THE PALETTE WRITE FILLER (§3.2)
```

COMPOSITOR keeps targets 0 and 1 and forwards 2..6 to PIXEL; **7 is decoded
nowhere**, which is exactly what makes it the filler.

Held colors persist until rewritten; `/RS` at vsync resets them to
`cmp_color1` = 0xFFF, `cmp_color0` = 0x000, abandons any mask playback,
a literal's buffer hold and the streaming flag, returns the source to
passthrough (never KEEP) and clears all machine state.  `/RS` also clears PIXEL's `pix_mode`,
`pix_base`, `pixel_skip` and `clut_index` to zero, so an untouched chip is 1bpp
full rate out of palette entries 0 and 1.  **The palette RAM is never reset by
anything** (§3.2).

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
only; whatever source was in force resumes, as if untouched, at pixel 16.
Two corollaries catch authors out: **dibit 00 is PASSTHROUGH — PIXEL's
RGB_IN — not "the span underneath"**, and **a mask laid over a LITERAL /
HOLD span resumes KEEP, which repeats the mask's LAST pixel, not the
literal's colour**: re-issue the LITERAL after the mask.

**The data word is never decoded.**  It is captured with `have_staged`
LOW, so no bit pattern of it can reach the SET/RUN decode or the PIXEL
forwarding bus.  A stray `01` therefore paints sixteen junk pixels and
eats the word behind it as dibits — frame-bounded, cleared by `/RS`, and
the price of a two-word record in a stream with no framing.

### 3.2 The palette — a 16 x 12 RAM and a THREE-WORD table entry

The palette is sixteen 12-bit entries in an external separate-I/O RAM (three
16x4 parts, 74F219 function table, `/CS` tied low).  **Its data inputs are
COMPOSITOR's `set_pix_value[11:0]`**; PIXEL supplies only the address and the
write pulse and taps `set_pix_value[3:0]` for its own registers.  The RAM's
outputs are COMPOSITOR's `RGB_IN`, combinational, costing no clock — which is
why no K constant moved.

**THE RAM IS NEVER RESET.**  Not by `/RS`, not at power-up: it comes up
undefined, software loads every entry it uses, and its contents persist across
frames.  (The model and the emulator fill it with reproducible **random** values
at power-up, so a display list that forgets an entry fails loudly instead of
reading a convenient black.)

**A table entry is three words**, and the third is not decoration:

```
SET pix_clut_index, i        the entry's address
SET pix_clut_data,  V        the colour, and the trigger for the write
SET 7 (filler),     V        the SAME twelve bits, to hold the bus
```

Sequence, in pixel clocks after the clock in which `set_pix_commit` is high for
the `clut_data` SET (clock 0), measured clock by clock by
`cpld/pixel/clut_pair_tb.v`:

| clock | `IDX` | `nCLUT_WE` | `set_pix_value` | what the RAM drives |
|---|---|---|---|---|
| 0 | the pixel index | high | V (the data SET's own payload) | the pixel's colour |
| 1 | **`clut_index`** | high | V — **setup** | the entry being written: wrong, but defined |
| 2 | `clut_index` | **LOW** | V (**from the filler**) | **undefined** (high-Z) |
| 3 | `clut_index` | high | V (from the filler) — **hold** | **undefined** (write recovery) |
| 4 | the pixel index | high | the next word's payload | the pixel's colour again |

The one-clock pulse is **centred in the three-clock address window**, so both
the address and the data are stable for a full clock before it falls and a full
clock after it rises.

Two rules for the list author, and both are measured, not advisory:

1. **HOLD — the filler is necessary, and target 7 is the only legal filler.**
   COMPOSITOR re-registers `set_pix_value` from **every** staged word whatever
   its target, and both its blanking and its active-video cadence stage one
   every TWO clocks: the data SET's own payload owns clocks 0 and 1, and the
   word behind it owns clocks 2 and 3.  Two words of ownership, three clocks of
   window, no slack in either direction.  Any other third word — another
   PIXEL-target SET, a COMPOSITOR colour SET — **stores its own payload in the
   entry** (measured, both cases), and a second `clut_data` starts a second
   mis-timed write.  An empty FIFO behind the data SET happens to work, because
   nothing disturbs the bus, but ENGINE's prefill makes that unpredictable: it
   is a measured behaviour, not a contract.  The word *before* the entry is
   unconstrained.
2. **WHERE — the disturbed window is the four pixel clocks 1..4.**  `IDX` is
   stolen for clocks 1, 2 and 3, so `RGB_IN` is wrong at clock 1 and undefined
   at 2 and 3; clock 4 is a guard for a write-recovery time nobody has read off
   a datasheet yet (§6).  Those four pixel slots must be **in blanking, or
   covered by a non-passthrough source** — a `cmp_color` RUN or STREAM RUN, a
   LITERAL/HOLD span, or an opaque MASK dibit.  A STREAM RUN over a colour
   register may carry the whole entry inside it (`clut_pair_tb` S6s); a STREAM
   RUN over passthrough may not (S6s-neg).  Clocks 1..3 are the entry's own three words; clock 4 is
   the first clock of the word after the filler, so **that word must not be the
   RUN that resumes passthrough**: leave one more record between them.  It may
   otherwise be anything at all, including a differently-valued SET — the write
   is over.

**Cost.**  Three words per entry, so a 26-word HBLANK packet reloads **eight
entries per line** with room for the mode / base / skip SETs, and a vertical
blank reloads all sixteen.  Photographic material wants two to five per line.

### 3.3 LITERAL, KEEP, HOLD and STREAM RUN (2026-10-03)

**LITERAL** is a one-slot playback record: it waits for H_ACTIVE like a RUN,
its slot shows its colour, and the source becomes **KEEP** afterwards.  The
colour is read out of COMPOSITOR's staged word during the clock after the
consume edge, so **the literal holds the on-chip buffer for its slot** — the
fetch parks the next word instead of capturing it.  That costs one clock and
is the record's defining law:

> **LL — a literal always paints at least two slots, and the record after it
> executes two slots later, whatever it is and whether or not it was banked.**
> A literal can be the SECOND record of a banked pair (it then lands on the
> slot after the first), never the first.

**KEEP** is RGB_OUT fed back: it repeats the last colour SHOWN, not a stored
colour.  After a literal that is the literal; after a passthrough span it is
the last playfield pixel (a smear); after a MASK it is the mask's last pixel.
RGB_OUT is black outside H_ACTIVE, so **KEEP at the start of a line shows
black** until a real source is loaded — a line must open with a real source,
and KEEP carried over from the line before shows black, not its colour.

**HOLD(N)** = KEEP for N slots.  **LITERAL + HOLD(N) is a flat span of any
colour in two words**, `max(N + 2, 4)` slots behind a long record: the
literal's slot, its KEEP slot, then N — except that the record after a
HOLD(1) is fetched too late to use HOLD's terminal, so HOLD(1) is as wide as
HOLD(2).

**STREAM RUN** sets a streaming flag; while it counts, a staged **SET**
executes on the next active edge without waiting for the terminal.  Laws:

* **Geometry is the run's.**  The run occupies exactly its count; the first
  non-SET record behind it waits for the terminal like behind any RUN.
* **Each SET executes at the ordinary cadence**, as if it were the record
  after a one-slot record: entry-edge commit, visible in its own slot.  With
  the run banked in HBLANK the first SET lands on run slot + 1 (the pair),
  then every two slots.  cmp_color1 under a colour1 stream run is a colour
  change at that slot; cmp_color0 is invisible until selected; PIXEL targets
  commit exactly as anywhere else (palette entries included, under the cover
  rule above).
* **Only SETs stream.**  A RUN, HOLD, LITERAL or MASK header behind a stream
  run waits for its terminal and blocks every SET behind it.
* **Leftovers.**  SETs not yet executed at the terminal execute as ordinary
  SETs, one slot plus the cadence HOLD each, and push everything after them
  later — the line overruns.  A run needs `2k` slots for `k` SETs.
* **A late SET recolours late; the geometry does not move.**  A late
  LITERAL, by contrast, delays everything after it.  That is what STREAM RUN
  is for.
* Streaming is an active-video rule: a SET behind a staged stream run in
  HBLANK is not eager.  The flag is reloaded by every RUN, STREAM RUN, HOLD
  and LITERAL and cleared by `/RS`.

**Compatibility.**  An old twelve-bit RUN of 1..1023 slots has `~count` bits
11:10 = `11` and is a valid RUN bit for bit.  An old RUN of 1024..2047 now
decodes as a STREAM RUN of `count mod 1024`, 2048..4095 as a HOLD, and the old
RUN(0) (4096) as 1024 slots; no well-formed line contained any of them.  Old
RUN_COLOR words are now LITERALs of an unrelated colour.

**Seam constants** (all measured by `compositor_tb.v`):

| sequence | value |
|---|---|
| literal pitch, sustained or out of a banked pair | 2 slots |
| literal -> next record (RUN of any source, STREAM, HOLD, MASK, SET of any target, LITERAL) | 2 slots; a SET's own slot and its cadence HOLD still show KEEP, so a SET behind a literal is visible from literal + 4 |
| MASK -> LITERAL | 0 (captured on the pixel-15 edge) |
| LITERAL + HOLD(N) behind a run | max(N + 2, 4) slots |
| three literals A B C banked in HBLANK | A 0-1, B 2-3, C 4-5, next record 6 |
| stream run banked in HBLANK -> streamed SETs | run slot + 1, then every 2 |
| streamed CLUT_DATA value held on set_pix_value | 4 clocks from its commit (rule needs 4) |
| RUN(0) | 1024 slots |

**Authoring rules** (what replaces RUN_COLOR):

1. A flat span of any colour is LITERAL + HOLD(N), two words, no register
   touched; or SET cmp_color + RUN, one word per span once the register is
   loaded.
2. One LITERAL = one 320-density pixel.  640-resolution per-pixel art stays
   MASK's job.
3. Open every line with a real source (KEEP after blanking is black).  A
   literal at the end of a line with nothing after it is a legal right border.
4. After a MASK over a literal span, re-issue the LITERAL.
5. A colour that changes mid-span at known x is a STREAM RUN over a colour
   register with SETs behind it; give it two slots per SET.
6. Counts are at most 1023.  Whole-frame passthrough is implicit HOLD, not a
   count.

## 4. Execution model

### 4.1 During active video (H_ACTIVE)

**Playback is one slot per clock.**  RUN, STREAM RUN and HOLD consume their
counts; a SET costs one slot; a LITERAL costs one slot but holds the buffer
for it, so it paints two (LL, §3.3); a MASK costs sixteen, its header's own
slot (pixel 0) included.  Inside a STREAM RUN a SET executes without waiting
for the terminal and its slot is one of the run's own (§3.3).

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
  line's first records, not a corner.  **A LITERAL cannot head a pair**: it
  holds the buffer, so the parked word is captured one edge late and
  executes two slots after it.

So *"consecutive SETs land on consecutive pixels"* — the old blanket
claim — now holds **only for a banked pair**.  Two SETs behind a RUN of
four or more still commit on adjacent pixels (that is what the
mid-line-split case relies on, and it is why the pair law exists); a
**third** SET in the same burst lands two pixels after the second, not
one.  compositor_tb measures the burst at slots 1, 3, 5.

### 4.2 Outside active video

* Playback idles — no slots, no counting.  Fetch continues at the same
  2-clock cadence (blanking does not exempt it): a staged SET executes
  immediately at pop, in stream order, and the first RUN, STREAM RUN, HOLD,
  LITERAL **or MASK HEADER** popped parks in staging and waits for H_ACTIVE, releasing
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
* A SET behind a staged STREAM RUN is not eager either: streaming is an
  active-video rule.
* No "wait" bits exist or are needed: the run-class records are the visible
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
  have to be priced.  A LITERAL contributes two slots wherever it sits
  (LL), LITERAL + HOLD(N) contributes `max(N + 2, 4)` behind a run, and a
  STREAM RUN contributes exactly its count provided it has two slots per
  SET behind it.
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
* **A mask over a LITERAL/HOLD span must be followed by the LITERAL again**
  (KEEP resumes as the mask's last pixel), and dibit 00 is passthrough, not
  the span below.
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
written, PIXEL re-delivers its reset word — all zeroes, which in 1bpp at
`pix_base` 0 is **palette entry 0** — so passthrough resolves to one palette
entry, which a three-word table entry can write.  Dibit 00 then behaves as a
**third settable colour**, available inside a group with no SET between groups.

> **This last paragraph is MODEL behaviour and is NOT yet verified against
> hardware.**  `super-engine/render.cpp` follows `pixel.v`'s documented
> tiling (no empty flag; a 7200 holds Q while empty, so an unfed stream
> re-delivers its last word), but what a 7200 pair presents on Q after
> `/RS` with no write at all, and what `pixel.v`'s shifter holds before its
> first fetch, are both open — and since 2026-09-20 there is a third: the
> palette RAM powers up undefined, so a pure-VIDCMD screen **must** load the
> entry its passthrough resolves to.  Settle all three before designing art
> around the third colour.

## 5. Cross-chip mechanism (the SET conduit)

**As built (decided 2026-08-13, `griffin.yml` `interfaces:` "COMPOSITOR
-> PIXEL SET register path"): a dedicated 12-bit registered value bus.**

* `set_pix_value[11:0]` — twelve registered traces carrying the staged word's
  payload, alongside `set_pix_valid` / `set_pix_target[2:0]` /
  `set_pix_commit`.  All four are registered off the same staged word, so they
  are one pipeline stage.  PIXEL applies the bus **directly at the commit
  pulse** — no shadow, no pending, no capture register.
* **Since the palette became a RAM the bus has two destinations**: all twelve
  bits go to the RAM's data inputs, and PIXEL taps only `[3:0]` (it has no
  register wider than four bits any more).  That is write form W2, and it is
  what puts the *filler* rule in §3.2 rather than a data path in PIXEL.
* **The bus is re-registered from EVERY staged word**, whatever the target.
  That is the mechanism behind §3.2's hold analysis: a payload owns the bus for
  two clocks and then the next word owns it.
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
| LITERAL visible at | its own slot (offset 0), and the slot after (KEEP) |

Semantics stay pixel-exact; K never varies at runtime.  What the rework
*did* change is the FETCH constants, which are not K: 2 slots per word
sustained, 1 slot of gap across a banked pair — and, since 2026-10-03, 2
slots after a LITERAL whatever follows it (§3.3's table).

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

**The palette RAM's budget is NOT settled, and it gates the part choice.**
Two arithmetics are owed, both against datasheet tables rather than estimates:

* **Read**, inside one pixel clock T = 39.72 ns: PIXEL tCO + flight + RAM
  address access + flight <= T − COMPOSITOR tSU.  With the numbers already
  pinned for the VIDCMD port (tCO 8.0, ~2 ns flight, T − tSU = 32.7) the RAM
  gets about **22.7 ns of address access** over 0–70 C.  A 27 ns part fails.
* **Write**: /WE pulse width against one clock, address and data setup and
  hold against the §3.2 sequence, and write recovery against §3.2's fourth
  guard clock — which is an abstraction of that unread parameter, not a
  measurement.
* Sixteen PIXELS inputs sampled on one GCLK edge is the same budget form as the
  VIDCMD port (tA 15 ns).

Board notes: series-terminate every /RE and /W at the driver end
(promoted from the rev-1 ringing bodge to design); PIXEL_CLK skew between
chips < ~2-3 ns (same net, matched routing) — one net fewer now that the raster
generator shares PIXEL's package.  PIXEL no longer senses /RE.

## 7. Chip summaries

### PIXEL (and the raster generator, in the same chip)

Drains PIXELS at 1, 2 or 4 bits per group; resolves each pixel to a FOUR-BIT
palette index; drives the palette RAM's address and write pulse.  Decodes no
instructions and holds no colour.  `timing.v`'s Timing module is instantiated
in the same top level, so the chip is also the raster time base for the whole
board.

Registers: mode (3), pix_base (4), pixel_skip (4), clut_index (4), the 16-bit
stream shifter with its bit position / alignment / half-phase / fetch state, the
attribute colour byte, the two-deep palette-write pipeline, the registered IDX —
plus the raster generator's counters and outputs.

Pins: PIXELS Q[15:0] in + one /RE out, IDX[3:0] + nCLUT_WE out,
**set_pix_value[3:0] in** (form W2 — the RAM takes all twelve from COMPOSITOR,
so PIXEL has no data path), set_pix_target[2:0] + set_pix_valid +
set_pix_commit in, and the raster outputs VGA_HSYNC, VGA_VSYNC, nRS, H_ACTIVE,
HBLANK, nVSYNC_IRQ, PADDLE_TICK, AUDIO_TICK.  43 of 64 I/O; the pin map lives in
`pixel.v`'s `//PIN` block and nowhere else.  DUART OP4..OP7 are reserved as
future mode inputs.

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
**FIT AS BUILT (2026-10-03, LITERAL + HOLD + STREAM RUN)**: 118/128 logic
cells, 92 flip-flops, 117/128 nodes+FB, 355 product terms, pass 1, tightest
logic block fan-in 34/40.  `cur_colour` (3) and two counter bits are gone,
`lit_active` and `streaming` (1 each) are new.  The literal's colour reaches
RGB_OUT through a new per-bit input, which is a logic-block FAN-IN problem,
not a product-term one: the old pinout put eight RGB_OUT pins in one block
already at 39/40, so the rev-2 pinout (PROPOSED, NOT FROZEN, in
`compositor.v`'s `//PIN` block) moves four of them to another block.  The fit
is at the mercy of the fitter — the same logic with declarations in another
order failed — so any RTL edit is a re-fit.

Pins: VIDCMD Q[15:0] in + /RE out + /EF in, RGB_IN[11:0], RGB_OUT[11:0],
**set_pix_value[11:0]** + set_pix_target/valid/commit out, timing in.
Pin-closed: 63/64 I/O + 3/4 dedicated inputs, one spare of each kind,
accepted because the interface is architecturally complete.

## 8. Signal table

| signals | path | function |
|---|---|---|
| PIXELS_Q[15:0] | PIXELS FIFO -> PIXEL | one ganged 16-bit word; each 7200 on its own eight pins |
| nPIXELS_RE | PIXEL -> both PIXELS 7200s | registered, one clock low per word, >= 8 clocks apart |
| IDX[3:0] | PIXEL -> palette RAM | the pixel's palette index, or `clut_index` during a write |
| nCLUT_WE | PIXEL -> palette RAM | one-clock write pulse, centred in the address window |
| RGB[11:0] | palette RAM -> COMPOSITOR RGB_IN | the pixel's colour, combinational |
| Q[15:0] | VIDCMD FIFO -> COMPOSITOR | instruction word (**no PIXEL tap**; high-Z between reads) |
| /RE | COMPOSITOR -> FIFO | registered, one clock low per word, 2 clocks per word sustained |
| /EF | FIFO -> COMPOSITOR | empty flag (2-FF sync'd rise, raw fall) |
| set_pix_value[11:0] | COMPOSITOR -> palette RAM data in; [3:0] -> PIXEL | the staged word's payload, registered |
| set_pix_target[2:0], set_pix_valid | COMPOSITOR -> PIXEL | "the bus is yours, register N" |
| set_pix_commit | COMPOSITOR -> PIXEL | apply the bus now (leads its slot's pixel by 1 clock) |
| RGB_OUT[11:0] | COMPOSITOR -> DAC | final color |
| PIXEL_CLK | oscillator -> PIXEL, COMPOSITOR | the one clock domain |
| H_ACTIVE, /RS | PIXEL -> COMPOSITOR (and /RS to the FIFOs) | blanking gate, per-frame reset |
| VGA_HSYNC, VGA_VSYNC, HBLANK, nVSYNC_IRQ, PADDLE_TICK, AUDIO_TICK | PIXEL -> the rest of the board | see `griffin.yml` PIXEL |

## 9. What 12-bit color costs elsewhere (summary)

* Zero memory bandwidth: pixel streams stay 1, 2 or 4 bits per group and
  indexed; literals ride SETs into the palette RAM.
* ENGINE: no change (word streamer).  The display-list builder writes wider
  literals and a three-word entry per palette colour; the VBLANK preamble
  re-establishes held colours, mode, base and every palette entry the frame
  uses — nothing in hardware remembers them across a power cycle.
* Board: 12 DAC pins + three 4-bit ladders; the 12-bit RGB bus now comes out of
  the palette RAM rather than PIXEL, PIXEL contributes IDX[3:0] + nCLUT_WE, and
  COMPOSITOR's twelve `set_pix_value` traces fan out to the RAM's data inputs as
  well as to PIXEL's four.
* Emulator/tools: 4-bit channel expansion (c*17), SET/target decode, the four
  depths, a palette unit with random power-up, r4g4b4 quantizer; super-engine
  host suite re-run.

## 10. Open items

Kept with their original numbering; status stamped rather than deleted.

1. ~~COMPOSITOR fit experiment~~ **CLOSED 2026-08-19**: 112/128
   macrocells as built, no counter-shrink or skip-merge lever needed.
2. ~~PIXEL fit~~ **CLOSED**: two palette entries, and shadow/pending are
   no longer adds — the value bus is applied at the commit (§5).
3. ~~RUN src=11~~ **CLOSED**: RUN_COLOR took the encoding; invert-run is
   the natural trade if the fit ever needs the room back.  **Superseded
   2026-10-03**: src 11 is LITERAL; RUN_COLOR is gone (§3.3).
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
   colour — passthrough resolving to one palette entry when the PIXELS FIFO is
   never written — is MODEL behaviour only.  Confirm against `pixel.v`
   what the shifter holds before its first fetch, and against the
   IDT7200 datasheet what Q presents after `/RS` with no write, before
   any art depends on it.  The entry itself must be loaded either way.
9. **NEW, OPEN, and it chooses a part.**  The palette RAM's read and write
   arithmetic (§6): a 74F219-pinout 16x4 part fast enough for ~22.7 ns of
   address access, its /WE pulse width, setup, hold and write recovery against
   §3.2's one-clock pulse, and the supply current of three of them.  Until it
   closes, §3.2's fourth guard clock stands in for the recovery time.
10. **NEW, OPEN, cheap and not needed yet.**  `clut_index` does not
   post-increment, so consecutive entries cost three words each rather than two
   (13 per HBLANK packet instead of 8).  It costs logic cells in a chip with
   five spare.
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
