# CLUT photo experiment

Question: if PIXEL loses micro-HAM and becomes index-only behind a 16-entry
CLUT reloadable per line through VIDCMD, do photographs survive?  Host-only;
no RTL, no suite model yet.  Background: griffin.log 2026-09-18 / 2026-09-19.

The answer is a table of error numbers plus a page of 1:1 images Brad can
look at.  The eye decides; the numbers rank.

## 1. Parts

Three programs and one shared header, all C++23, in this directory.

    clut_format.h     the stream format (§3), plain structs, read/write
    clut_prepare      PPM -> 640x480 reference PNG (and nothing else)
    clut_encode       reference PNG + mode + knobs -> .clut stream file
    clut_decode       .clut file -> PNG; also validates the stream (§3 rules)
    clut_compare      reference PNG + decoded PNGs -> metrics CSV / markdown
    run.sh / Makefile the sweep (§5) and the report page

The decoder shares clut_format.h with the encoder and NOTHING else: it must
not include the encoder's simulation.  Metrics are computed only from decoder
output, never from the encoder's own prediction.

## 2. Candidate modes

All at 480 lines.  "Bandwidth" is PIXEL words per line; micro-HAM today is 80.

| id      | pixels            | words/line | meaning                                   |
|---------|-------------------|-----------:|-------------------------------------------|
| mham    | 640, micro-HAM    | 80 | baseline, existing `../microham_encode` (ordered and fs) |
| c4h     | 320 x 4bpp, half rate | 80 | equal-bandwidth contender                  |
| c2f     | 640 x 2bpp, full rate | 80 | equal-bandwidth contender, 4 of the 16 entries per line |
| c4f     | 640 x 4bpp, full rate | 160 | ceiling; DMA bandwidth NOT verified, report it flagged |
| attr    | 320, {fg4,bg4,pattern8} per 8 source pixels | 40 | the tile mode asked to show a photo |

Bits are consumed MSB-first from big-endian 16-bit words, as micro-HAM does.
Half-rate modes are decoded by pixel doubling to 640 before comparison, and
their source is the 640 reference box-averaged to 320.

Knobs, each swept:

* K = CLUT entries reloaded per line: 16 (post-increment index), 12 (index +
  colour pairs in a 26-word hblank packet), 4.  Entries not reloaded persist
  from the previous line.  Line 0 may load all 16 regardless.
* N = refinement spans per line: 0, 4, 8, 16.  A span paints a constant
  R4G4B4 colour over the pixel stream (COMPOSITOR SET cmp_color + RUN,
  vidcmd-spec.md §3).  Model: 3 VIDCMD words per span, span length >= 4
  output pixels, gap between spans >= 4 output pixels, spans do not overlap,
  total in-line words <= 147.  This placement rule is a conservative stand-in
  for the planner recurrence; say so in the report.
* Dither: none, ordered (reuse the blue-noise mask and amplitude convention
  from ../microham.h), Floyd-Steinberg.

## 3. Stream file (.clut)

Header: magic, mode id, width, K, N.  Then per line:

    load_count, load_count x { index u8, colour u16 R4G4B4 }
    span_count, span_count x { x u16, length u16, colour u16 }
    pixel words (big-endian u16, the mode's words/line)

Validity rules the decoder enforces, exiting non-zero with the line number:
load_count <= K (line 0: <= 16), indices < 16 and unique within a line,
span rules of §2, exact word count.

## 4. Encoder

Heuristic is fine; the decoder is the truth.  Suggested shape, improve freely:

1. Per-line palette: k-means / median cut in R4G4B4 over the line's pixels,
   weighted with the lines directly above and below (reduces horizontal
   streaking).  For K < 16, start from the previous line's CLUT, match new
   centroids to old entries, replace the K entries that reduce error most.
2. Index assignment with the chosen dither, error measured against what the
   decoder will show.
3. Spans: per line, a DP choosing up to N non-overlapping spans (length
   limits above) that maximise squared-error reduction against the
   UNDITHERED target; span colour = quantised mean of the target under it.
4. attr mode: per 8-pixel cell choose the best pair from the line's 16 and
   the 8-bit pattern (ordered dither allowed inside the cell).

## 5. Comparison and report

Metrics, identical to the two printed by ../microham_encode so old and new
numbers line up: mean per-pixel squared RGB444 error, and mean squared 8-bit
error of 4x4 box means.  Add PSNR (8-bit) for convenience.

Sweep: 4 images (../../../reference-images/{maya2,millie2,adobe2,oops2}.ppm)
x modes x K x N x dither, pruned sensibly (mham has no K/N; sweep N only at
K = 16 and the best dither per mode if the full product is slow).

Outputs under out/ (git-ignored): decoded PNGs, results.csv, results.md
(one table per image, best-per-mode summary on top), and index.html showing
for each image the reference and the best variant of each mode at 1:1, with
captions carrying mode, K, N, dither, words/line and both metrics.

clut_prepare: scale to cover 640x480 preserving aspect, centre crop, a
decent filter (area average or better), in 8-bit sRGB without gamma games.
The same reference PNG feeds microham_encode and clut_encode.

## 6. Gates

* `make check`: for every swept file, decoder output == the encoder's
  predicted image byte for byte (the encoder writes its prediction only for
  this test); the validator accepts every swept file; three deliberately
  corrupted files (K exceeded, overlapping spans, short pixel data) are each
  rejected.
* c4f with K=16, N=0, dither none on a synthetic image of <= 16 colours per
  line reproduces it exactly.
* Whole sweep under 15 minutes at -O3 on this Mac.
* Makefile assigns CXXFLAGS with `:=` (the environment exports an older -std).
* Style: ../../c++-style.md — Allman braces, braces on every block, 4 spaces.
