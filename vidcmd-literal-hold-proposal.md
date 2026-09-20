# VIDCMD proposal: LITERAL and counted HOLD replace RUN_COLOR

Status: PROPOSAL 2026-09-20 (Brad's ideas, worked through against
`cpld/compositor/compositor.v`).  Nothing here is fitted, simulated or
adopted.  To be run as a COMPOSITOR fit ladder after the indexed-PIXEL change
(packages 1-3) and the Smurf trial.  Related `griffin.log` entries: 2026-09-20
"LITERAL replaces RUN_COLOR".  If adopted it is a semantic COMPOSITOR change
and goes through all five video-chain layers (CLAUDE.md).

## 1. Command set

| record | encoding | status |
|---|---|---|
| RUN passthrough / cmp_color1 / cmp_color0 | `{00, src, ~count}`, src = 00 / 01 / 10 | unchanged; count narrows 12 -> 10 bits |
| MASK | `{01, d1..d7}` + `{d8..d15}` | unchanged |
| SET | `{1, target[2:0], value[11:0]}` | unchanged word format |
| SET 0, 1 | cmp_color1, cmp_color0 (swapped numbering stays) | unchanged |
| SET 2..6 | clut_index, clut_data, pix_base, pix_mode, pixel_skip | landed with indexed PIXEL; COMPOSITOR only forwards |
| SET 7 | palette-write filler, ignored by both chips | landed |
| RUN_COLOR | `{00, 11, colour[2:0], ~count[8:0]}` | **REMOVED** |
| LITERAL | `{00, 11, colour[11:0]}` — one R4G4B4 pixel per word | **NEW**, takes RUN_COLOR's prefix |
| HOLD(N) | `{00, src != 11, 0, x, ~count[9:0]}` — a RUN with bit 11 LOW | **NEW** |

Why HOLD can hide in RUN: counts are stored complemented and a line has at
most 640 slots, so every RUN in every existing list already has bits 11:10 =
`11`.  Bit 11 low is free to mean "keep the last output colour for N slots";
the src field is then ignored.  Existing RUN words stay valid bit for bit.
`compositor.v` already forces RUN_COLOR's unused top count bits to ones so one
terminal compare serves both encodings (`w_load`); HOLD and the narrowed RUN
use the same trick with `{2'b11, staged_word[9:0]}`.

`pix_mode` value bit 3 stays reserved for the single-pin 1bpp bypass
(`cpld/experiments/indexed-pixel`, r9direct); unrelated to this proposal.

## 2. COMPOSITOR operation

`cur_src` keeps four values.  Three are unchanged (passthrough, cmp_color1,
cmp_color0).  The fourth changes from "RUN_COLOR colour" (`SRC_COLOUR`, fed by
the 3-bit `cur_colour` latch) to **KEEP: RGB_OUT holds its value**.

* LITERAL: a one-slot record.  Its slot shows `staged_word[11:0]`; on the next
  slot `cur_src` is KEEP.
* HOLD(N): `cur_src` = KEEP for N slots.
* Implicit HOLD (count terminal, nothing staged) is unchanged: it continues
  the current source, which may now be KEEP.
* RUN, MASK, SET, the fetch engine, the banked pair, eager blanking SETs, /RS:
  unchanged.

### Where the literal's colour lives (the one real obstacle)

The colour mux is combinational from post-edge state and RGB_OUT registers it
one clock later.  When a record is consumed, `staged_word` may be replaced on
the same edge by the banked next word, so a literal's 12 bits would be gone
before they reach RGB_OUT.  RUN_COLOR solves this with the 3-bit `cur_colour`
latch; a 12-bit latch is +9 FF and is not expected to fit.

Proposed: **a LITERAL holds the bank for its slot, the way MASK already does**
— `staged_word` is not replaced during the literal's slot and the mux reads
the colour straight from it.  No colour latch at all.

Consequences:

* Literals never land on adjacent 640-resolution slots; consecutive literals
  are always >= 2 clocks apart, and the record after a literal arrives >= 2
  slots later.  (An earlier claim that a banked pair of literals gives two
  adjacent 640-res pixels is WRONG under this implementation.)
* At the sustained 2-clock fetch cadence a literal is exactly one half-rate
  (320-density) pixel: its own slot plus one slot of implicit HOLD.

### Expected logic delta (estimate, unfitted)

COMPOSITOR is 122/128 LC, pin-closed, PT-dense on the twelve RGB_OUT cells.
In: one literal term and one keep-feedback term per RGB_OUT cell, the
bank-stall decode, the HOLD flag decode.  Out: the RUN_COLOR colour term per
cell, `cur_colour` (3 FF), two counter bits (2 FF), the 9-bit `w_load` special
case.  Net about +1 PT per RGB_OUT cell and about -5 FF.  History: an override
adding 3 PT per cell did not fit; the bit-mode mux adding about 2 did (118 LC).
Only a ladder settles it: rung 1 LITERAL alone, rung 2 + HOLD, rung 3 counter
narrowed to 10 bits.

## 3. Authoring rules

Existing rules stand (each line's records occupy exactly 640 slots as
executed; a SET pair needs >= 4 px of playback in front of it to land on
adjacent pixels; an overrunning record steals the next line's eager SETs; the
palette-write entry is three words and its clocks 1..4 need blanking or
non-passthrough cover).  New:

1. LITERAL = one 320-density pixel (2 clocks); consecutive literals sit at
   2-clock pitch.  Full-resolution per-pixel art stays MASK's job.
2. Flat span of any colour: LITERAL + HOLD(N), 2 words, no register touched.
   SET cmp_color + RUN still works and is 1 word per span once the register
   is loaded.
3. KEEP does not survive blanking: RGB_OUT is black there, so a line must
   open with a real source.  A literal at the end of a line followed by
   nothing is a legal right border (implicit HOLD to the end of the line).
4. After a MASK, KEEP repeats the mask's LAST PIXEL, not the colour that was
   underneath: re-issue the LITERAL.  (Same repaint rule as MASK over
   RUN_COLOR today.)
5. HOLD directly after passthrough smears the last playfield pixel.  Legal.
6. RUN and HOLD counts are at most 1023.  Whole-frame passthrough is implicit
   HOLD, not a count, so nothing needs more.

## 4. What it is for

* RGB444 sprites for the 320-wide game modes: each sprite row a precompiled
  word array — one LITERAL per coloured pixel, HOLD(N) for same-colour runs
  (wins at >= 3 game pixels), RUN(passthrough) for transparent gaps.  With the
  background in PIXELS, a line program is gap RUN + row + gap RUN + row ...:
  concatenation, not splicing.  Depth overlap = slicing the back sprite's row
  by index (trim a HOLD/RUN count at the cut).  Rows are compiled for a known
  entry state (preceded by a >= 4-px run); `render/plan.h` in the Smurf port
  can verify.
  Cost: ~16 words per 16-px row against 2 for a two-colour MASK row; about 100
  literal pixels per line over a 40-word attribute background inside the
  147-word budget.  Zero-copy variant: descriptors point at row data in place
  and X is the one gap-RUN count word; ~14 SYSCLK descriptor overhead per
  fragment.
* Patch pixels over a 4bpp photograph: ~50 exact-colour half-rate pixels per
  line beside 80 pixel words.  `image-tools/clut-experiment` can evaluate it
  with a literal-pixel span model.
* A true-colour video window over a full-rate desktop (Brad, 2026-09-20):
  PIXEL shows a 640-wide 1bpp or 2bpp framebuffer; on the window's lines the
  program is RUN(passthrough, X) + N literals + RUN(passthrough, rest).  The
  window is 2N screen pixels wide (even width, any X with >= 4 px of run in
  front).  Budget: ~147 words/line minus 80 (2bpp) leaves ~60 literals = 120
  screen px; over 1bpp (40) ~100 literals = 200 px.  Descriptors can point at
  the frame data in place and flip between two buffers in the vsync ISR, so
  display costs no copying; filling the buffer (e.g. raw RGB444 words from CF,
  60x45 at 15 fps = ~40 KB/s) is the CPU's job with roughly 45% of the machine
  left on a 2bpp screen (cost-model estimate, not benchmarked).
  Horizontal zoom = LITERAL + HOLD(n) per video pixel, width about 2 + n
  clocks (4 minimum; to be measured): 2x costs the same words per screen pixel
  as 1x, 3x saves a third, 4x half.  The stream is [LIT, HOLD, LIT, HOLD ...]
  with the HOLD words written once and the CPU updating literals at stride 2.
  Vertical zoom = several lines' descriptors naming the same row.
* Any-colour vector dots and short segments (Tempest) in 1-2 words.
* Bandwidth ceiling with PIXELS idle and ~10% CPU (estimate from the
  `descriptor.h` cost model, 2.44 SYSCLK per word in full descriptors): ~164
  literals per line = 160x480 true colour across the full width at 4-clock
  pixels (LITERAL + HOLD(1), or a future width field), or a half-width window
  at half-rate density.  The 256-word FIFO is not the limit; DMA delivery is.

## 5. What is lost, and what must be rebuilt

* The one-word span in the eight saturated corner colours (now 2 words).
* RUN_COLOR words become literals, so every list using them is re-authored:
  `apps/dtest` (white square, yellow paddle bar), `apps/smurf` +
  `image-tools/sprite2spans` (corner-colour sprite spans),
  `apps/dmabench` (black fill), the griffin-ports Smurf app's borders, suite
  case m7-kiosk's highlight bar; `compositor_tb.v`'s RUN_COLOR checks.
* Five layers: `compositor.v` + header laws, `compositor_tb.v` (102 checks;
  new measured constants for literal pitch and the seam after a literal),
  `super-engine/render.{h,cpp}` CompositorUnit and `plan` recurrence (a
  literal is a bank-holding record like MASK), suite cases, emulator;
  `vidcmd-spec.md` section 3/4, `griffin.yml` constants, the datasheet page
  (its "LITERAL" ADVANCE section is superseded by this file).

## 6. Open questions

* Does it fit?  (Section 2 ladder.)
* Exact seam constants after a literal and between a literal and a SET / MASK,
  to be measured by the testbench, not derived here.
* Whether HOLD's flag can be bit 11 alone or needs bits 11:10 = `0x` checked;
  what a RUN word with bits 11:10 = `10` means (reserve it).
* Whether a width field for literals (4-clock pixels in one word) is worth
  any logic, given LITERAL + HOLD(1) already does it in two.
