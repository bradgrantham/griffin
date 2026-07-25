# Video enhancement ideas — bit depths, external LUT, component-wise modify

Design notes from 2026-07-24/25 discussion, following the ENGINE 2-cycle
streaming DMA change (commit 934f7a2).  Nothing here is committed work; this
is the menu of what that change made possible, with the constraints mapped
out.  Per project rules, any VIDEO change starts with a fit experiment before
other source is touched.  ENGINE-side ideas (copy accelerator, transparency
writes) are in the companion docs/engine-blit.md.

## What the streaming DMA unlocked

Bus load at ~2.65 effective SYSCLKs/word (2 transfer cycles + arbitration
amortized over 20-word bursts):

| Mode          | Bytes/line (incl. 4-byte header) | Bus load | FIFO /RE period |
|---------------|----------------------------------|----------|-----------------|
| 1bpp 640 (today) | 84                            | ~23%     | 318 ns          |
| 2bpp 640      | 164                              | ~45%     | 159 ns          |
| 4bpp 320 (pixel-doubled) | 164                   | ~45%     | 159 ns          |
| 4bpp 640      | 324                              | ~88%     | 79 ns           |
| 8bpp 320      | 324                              | ~88%     | 79 ns           |

2bpp@640 was arithmetically impossible at the old 7 clocks/word (words needed
exceeded total bus cycles per frame).  The ~88% modes are technically feasible
but starve the CPU and ride the FIFO refill margin — stunt modes at best.
Faster /RE and /W rates worsen the rev-1 bodge-wire ringing problem
(see project_fifo_re_ringing); rev 2 should source-terminate these.

## Division of labor (established principle)

* **VIDEO owns horizontal interpretation**: bpp, palette/LUT, pixel rate,
  line format, header consumption.
* **ENGINE owns memory traversal**: base address, wrap point, and *which*
  words in *what order*.  ENGINE is data-blind; consumption rate is emergent
  through nFIFO_HF flow control.
* Modes that change what a word *means* are free for ENGINE.  Modes that
  change *which* words or *how many times* (interlace field stride, vertical
  line-doubling — the FIFO is consume-once and VIDEO has no line buffer) are
  ENGINE work: rewind/stride address generation.
* ENGINE per-mode needs: the wrap constant (mode-bit muxed constants, or a
  CPU-writable frame-length register ~16 FFs to make ENGINE mode-agnostic
  forever), and for frames > 64KB the address re-slice
  `{source_page[7:0], word_counter[14:0]}` → `{source_page[6:0],
  word_counter[15:0]}` (same FF count; framebuffer alignment becomes 128KB).
  A 2bpp@640 frame is 39,360 words = 78.7KB, so it needs the re-slice.

## 2bpp (4 colors, per-line palette)

* The existing 4-byte line header carries exactly 4 palette entries: bytes
  0–1 (today's fg/bg) plus the reserved bytes 2–3 become entries 2 and 3.
  No stride-structure change; per-line 4-color palettes fall out for free.
* Internal-palette attempt on 2026-05-11 failed to fit: "Grouping fail" from
  LAB fan-in on the 4:1 color mux, not raw macrocell count (see
  project_cpld_fitter_pinout).  Pipelining the mux in two 2:1 stages was the
  proposed fix; the external LUT (below) eliminates the mux entirely and is
  the better path.
* VIDEO changes otherwise modest: mode register, shift-by-2, byte_cnt to
  8 bits, 4-entry palette (+16 FFs if internal).

## 4bpp (16 colors)

* A 16-entry × 8-bit CLUT in CPLD FFs is flatly impossible: 128 FFs = the
  entire ATF1508.  Options:
  * **Direct color**: fixed combinational nibble→R3G3B2 decode (IRGB /
    EGA-style 16 fixed colors).  No FFs, cheap, no board change.  The
    "good enough" fallback.
  * **External palette RAM** — see next section.  The real answer.
* 4bpp@320 (pixel-doubled) is the sweet spot: same bus load as 2bpp@640.

## External palette RAM ("the LUT")

Chosen topology (option 2 from discussion): **RAM in a feedback loop through
VIDEO**, not RAM-drives-DAC-directly.

    VIDEO index out (4–8 bits) → RAM address
    RAM DQ → VIDEO input pins → modify/accumulate → output FFs → RGB → DAC

* Keeps the output FFs in VIDEO, which preserves the accumulator needed for
  component-wise modify modes (below) and lets indexed and modify modes
  coexist in one bitfile.
* The RAM becomes general attached memory on VIDEO in principle, but VIDEO's
  capacity realistically limits it to LUT duty — and even that needs a fit
  experiment; the pin budget is the ragged edge (below).
* **Write path**: staged single-entry write.  VIDEO latches one pending
  {index, data} (~13 FFs) from a normal CPU register write, commits to the
  RAM during the next hblank.  RAM /OE tied low; /WE-controlled writes (DQ
  goes input while /WE low).  The DQ net is shared: VIDEO drives it during
  hblank writes, RAM drives it during active scanout.
* **Copper trick**: with a writable LUT, the reserved header bytes 2–3 can
  carry an in-band {index, value} — one palette entry auto-updated per
  scanline by the DMA stream itself, zero CPU, beam-synchronous.  Raster
  bars / per-band palettes from the framebuffer layout alone.  (Mutually
  exclusive with using those bytes as 2bpp entries 2/3, per mode.)
* **Parts**: bin has KM681000BLP-7 ×2 (128K×8, 70 ns).  70 ns closes only at
  320-wide index rates (index changes every 79.4 ns — knife-edge with CPLD
  Tco; scope it).  640-wide lookup needs a ~15–25 ns part (74S189/F219 pairs
  or fast 6116-class, purchase).  A byte-wide RAM gives a 256-entry CLUT →
  8bpp@320 exists in hardware for free once wired, bandwidth permitting.
* **Pin budget**: micro-HAM VIDEO build is 53/64 I/O.  Feedback topology
  needs ~13 more (4–8 index/addr + 8 shared DQ + /WE) → ~2 short at 4-bit
  index; recoverable by sacrificing CPST_CLK_ENB (held off anyway) and
  VIDEO_STALL (fitter anchor) with pull-resistor bodges.  Needs a real
  pin/fit pass before believing it.
* **Logic budget**: removing the internal palette FFs and 4:1 mux should
  more than pay for the staging FFs and pass-through; the whole "palette
  pressure" axis in VIDEO evaporates.
* Rev 1 retrofit means lifting the 8 DAC ladder connections and
  breadboarding the RAM (same spirit as the FIFO bodge, but touches the
  analog path).  Cleaner as a rev-2 design-in: one socket + ~13 routed nets.

## Component-wise modify (née "differential color" / micro-HAM)

* Fundamental dividing line: **a LUT is memoryless; modify modes are
  stateful.**  The current color must survive pixel-to-pixel in a register
  in the pixel path.  In today's internal design that register exists for
  free — the 8 VGA output FFs are the accumulator (what the uncommitted
  micro-HAM experiment exploits, 125/128 LC with xor_synthesis).
* The feedback LUT topology above is what lets the LUT and the accumulator
  coexist; RAM-drives-DAC-directly would forfeit modify modes.
* "Component-wise modify" = hold the accumulator, replace one component
  field (R/G/B) per op from the pixel stream.  True *differential* (signed
  per-component deltas) needs adders — CPLD territory, and likely too rich
  for the remaining real estate; treat as aspirational.
* **Accumulator initialization**: no CPU write port wanted — mid-frame CPU
  writes race the beam.  Load the accumulator from the line header at line
  start: deterministic, beam-synchronous, bounds error propagation per line,
  and reuses machinery the palette-and-pixels layout already provides.

## Open questions / order of operations

1. Fit experiment: VIDEO with 2bpp + 4-entry internal palette (retry of
   2026-05-11 with the two-stage mux), vs. jumping straight to the feedback
   LUT pinout study.
2. Scope the KM681000-7 timing margin at 159 ns and 79 ns index rates.
3. Decide rev-2 DAC topology (RAM-fed vs CPLD-fed) — CPLD-fed is the
   flexible fork and the current preference.
4. ENGINE frame-length register vs. muxed constants; address re-slice.
5. Emulator: mode plumbing + LUT model when any of this lands.
