# OVERLAY — RLE/masked span compositor (design summary, 2026-07-30)

Status: design sketch from the rev-2 expansive phase.  Not yet in griffin.yml,
no Verilog, no fit results.  Companion to the ENGINE descriptor-walker and
TIMING/PIXEL split proposals.

## Purpose

Hardware pointer/sprite overlay for the VGA pixel pipeline.  The CPU builds
per-scanline span lists; ENGINE DMAs them into a dedicated FIFO during HBLANK;
a small dedicated CPLD composites them over the PIXEL output at pixel rate.
No CPU work at pixel or line deadlines — consistent with the project's
hardware/software split doctrine.

## Position in the pipeline

```
                 PIXELS FIFO (9-bit)         OVERLAY FIFO (16-bit, 2x 7200)
                       |                            |
  TIMING (1504) --> PIXEL ----RGB332----> OVERLAY ----RGB332----> DAC resistors
   (VGA counters,   (shifter, CLUT,      (span compositor)
    sync/blank,      2bpp, in-band
    wait events)     palette)
```

* OVERLAY is a **pipeline stage downstream of PIXEL**: RGB332 in, RGB332 out,
  one pixel of latency (invisible; pre-compensate in TIMING if desired).
* PIXEL never knows OVERLAY exists — so 2bpp and overlay coexist by
  construction, and the stage is severable: bypassable with jumpers or simply
  not stuffed on a rev-2 board.
* Inputs from TIMING: pixel clock (25.175 MHz), HBLANK (line reset).
* Fed by its own FIFO: two 7200s in parallel for a 16-bit word (9th bits
  unused).  ENGINE fills it with whole bus words — no 16-to-2x9 byte
  serialization as on PIXELS; potentially fly-by (bus read cycle with
  OVERLAY /W strobed, one bus cycle per word).

## Record format

The FIFO carries a sequence of variable-length records, consumed left to
right across the scanline.  Each record begins where the previous one ended
("count starts immediately"); there is no X coordinate field, so records are
relative and the CPU emits them sorted in X.

### Word 0 (always present)

| bits  | field | meaning                                   |
|-------|-------|-------------------------------------------|
| 15    | SOLID | 1 = solid run, no mask word follows       |
| 14:8  | COUNT | run length in pixels; 0 means 128         |
| 7:0   | COLOR | R3G3B2 overlay color                      |

### SOLID = 1 — solid run (16-bit record)

Draw COUNT pixels of COLOR.  Single-pixel change = COUNT of 1 (16 bits).

### SOLID = 0 — masked run (32-bit record)

The next FIFO word is a 16-bit mask.  **The record's first pixel is
structurally transparent** — the mask word is popped during that pixel and
first consulted on the second, which is what makes back-to-back record
timing close (see pop budget).  From the second pixel on: mask bit
1 = replace with COLOR, 0 = passthrough (PIXEL's RGB unchanged), consumed
leftmost-first (bit 15 → record pixel 2) and **recirculating** every 16
pixels when COUNT is large — 32 bits buys up to a 128-px stipple/dither/
hatch fill (127 maskable pixels after the lead-in).  When COUNT expires the
mask ends early (partial consumption) and the next record starts
immediately.

No expressiveness is lost: a pattern that starts opaque peels its leading
opaque run into a solid record, after which the pattern's first transparent
pixel aligns with the masked record's lead-in.  Emitter rule: a masked
record always begins on a transparent pixel.  In the CPLD the lead-in is
just the mask-pending state outputting passthrough.

This works for repeating fills too, because rotation preserves the period:
peel the leading opaque run of length k as one solid, then the mask is the
pattern rotated left by k.  E.g. 0xAAAA repeated N px = solid(COUNT=1) +
masked(COUNT=N-1, mask=0x5555) — 48 bits regardless of N, one extra solid
per fill start, not per repeat.  When chaining past 128 px, each
continuation record's first pixel must also be transparent: the emitter
picks the previous COUNT (127 vs 128) to land the boundary on a 0 bit, or
inserts another peel solid.

### Passthrough gap

SOLID = 0, mask = 0x0000: skip up to 128 px in 32 bits.  Crossing a full
640-px line takes at most 5 chained gap records.  There is deliberately no
16-bit gap record; trailing gaps are free (see /EF below).

## Line framing and shear containment

RLE streams shear catastrophically on any slip, and variable-length records
make framing corruption worse — so desync is contained to one line:

* **Reset-then-fill**: the ENGINE descriptor that fills OVERLAY carries a
  control-word bit that **pulses the FIFO /RS before filling** — atomic under
  ENGINE control, no cross-chip race with TIMING.  One fill descriptor per
  line that has overlay content, placed after that line's wait-HBLANK
  descriptor.
* **/EF = done for this line**: when OVERLAY finds the FIFO empty at a record
  boundary, it latches "done" and passes through for the rest of the line
  (re-armed by HBLANK).  Consequences: no end-of-line token, no padding
  counts out to 640, and a line with no overlay costs zero tokens and zero
  DMA.
* A corrupted flag bit therefore garbles at most one scanline, once.

## Pop budget

OVERLAY can pop at most **one FIFO word per pixel clock** (40 ns).  Sustained
worst case — a chain of 16-bit records — is 1 pop/pixel, so the FIFOs must be
a ≥25 ns read-cycle speed grade (verify the bin parts).

The transparent lead-in on masked records makes every legal stream
timing-feasible with a uniform one-ahead prefetch:

* During any record's **last pixel**: pop the next record's word 0.
* During a masked record's **lead-in pixel**: pop its mask word.

Every pixel needs at most one pop — including the once-pathological case of
a 1-px solid of color A followed by a color-B pattern (word 0 pops during
the A pixel, mask pops during the lead-in pixel).  Without the lead-in rule
that case needed two pops before the pattern's first pixel and had no
general fix: absorbing the A pixel into the B record's mask fails when the
colors differ.

Remaining emitter rule: masked records use COUNT ≥ 2 (COUNT = 1 would render
nothing and break the one-pop budget; a 1-px change is the 16-bit solid
form).

## CPU responsibilities

Assemble per-line span arrays in RAM (the ENGINE frame list DMAs them):

* Sort spans in X; clip to the line; merge adjacent same-color solids.
* Chop runs and gaps at 128 (COUNT encoding 0 = 128).
* Encode gaps as mask=0 records; omit the trailing gap entirely.
* Start masked records only on transparent pixels (peel leading opaque
  pixels into solids); COUNT ≥ 2 for masked records.

Worked example — outlined cursor row `B W W W W B`:
gap(32) + B solid(16) + W solid(16) + B solid(16) = **80 bits**.
A 16-row pointer at 3–5 records/line is roughly 200 bytes of HBLANK DMA per
frame — noise on the bus budget.

## DMA-ahead constraint

Fill rate (~7 Mwords/s at 14 MHz, 2-cycle bursts) is below the worst-case
drain (25 Mwords/s for dense 16-bit-record chains), and once /EF is seen the
line is over — a late fill doesn't resume it.  So per-line overlay content
must be small enough that the fill stays ahead of consumption; typical loads
(cursor, few sprites) complete within HBLANK.  Long dense span lists on one
line are the hazard: keep them modest or accept truncation.

## CPLD implementation notes

Working set: COLOR (8) + COUNT up-counter (7, loaded with ~COUNT, all-1s
terminal per the ATF constant-cost rule) + SM (~3) + RGB pipeline register
(8) + staging.  Two datapath options:

1. **Mask-on-bus (preferred if it makes timing)**: no mask shifter at all.
   The mask word sits on the FIFO output bus for the record's duration and a
   16:1 mux indexed by the count's low 4 bits selects the current bit — the
   FIFO's own output register is the mask storage, and recirculation falls
   out free.  Saves ~16 macrocells at the cost of mux product terms.  The
   transparent lead-in pops the mask a full pixel before its first bit is
   used, so the FIFO's ~15 ns t_A is out of the per-pixel critical path —
   just stable-bus → 16:1 mux → output register in 40 ns.  Plausibly fits
   an **ATF1504**.
2. **Real 16-bit shifter (fallback)**: recirculating shift register if the
   mux path misses timing.  Wants an **ATF1508**.

## Open items

* FIFO speed grade of the bin 7200s (need ≥25 ns read cycle).
* Mux-vs-shifter timing check; 1504 vs 1508 decision by fit.
* Pin assignment (new chip, rev-2 layout): 16 FIFO D in, /R, /EF, /RS
  routing (driven by ENGINE), 8 RGB in, 8 RGB out, pixel clock, HBLANK.
* Source-terminate the new FIFO strobes at layout (rev-1 /RE ringing lesson).
* Bypass provision (jumpers or not-stuff) so rev 2 works without the stage.
* Formalize in griffin.yml (record format constants, ENGINE control-word RS
  bit) once ENGINE's descriptor walker fits.
