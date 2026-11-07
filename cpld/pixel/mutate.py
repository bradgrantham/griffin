#!/usr/bin/env python3
"""mutate.py — negative controls for pixel_tb.v.

`make pixel-mutants` runs the production bench against each file this writes
and REQUIRES every one of those runs to fail.  A bench that cannot fail is not
a specification.

Each mutant is ONE plausible mistake, applied to the `Pixel` module taken
verbatim out of pixel.v, so a mutant can never drift from the RTL: if a
refactor moves an anchor, this script stops with an error instead of silently
emitting a mutant that is identical to the original.

Only the Pixel module is emitted (the bench instantiates nothing else), so the
mutants do not need timing.v and cannot be mistaken for a buildable chip.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "pixel.v")
OUT = os.path.join(HERE, "mutants")

src = open(SRC).read()

start = src.index("\nmodule Pixel\n")
end = src.index("\nendmodule\n", start) + len("\nendmodule\n")
pixel = src[start:end]

MUTANTS = [
    ("mut1", "the two halves of the word exchanged at the load (BYTE ORDER)",
     "            shift_reg <= PIXELS_Q;",
     "            shift_reg <= {PIXELS_Q[7:0], PIXELS_Q[15:8]};"),

    ("mut2", "the skip preload off by one (bit_pos <= skip_fine + 1)",
     "            bit_pos <= skip_fine;",
     "            bit_pos <= skip_fine + 4'd1;"),

    ("mut3", "the end-of-line fetch cutoff removed entirely",
     "    wire fetch_cutoff = fetch_last & (extra_fetch_done | ~line_skipped);",
     "    wire fetch_cutoff = 1'b0;"),

    ("mut4", "the attribute colour latched on the load edge, one clock early",
     "            if (col_transfer)\n"
     "            begin\n"
     "                attr_colour <= shift_reg[15:8];\n"
     "            end",
     "            if (d_attr & fifo_loading)\n"
     "            begin\n"
     "                attr_colour <= PIXELS_Q[15:8];\n"
     "            end"),

    ("mut5", "the skipped line's one-word allowance removed (record, not record+1)",
     "    wire fetch_cutoff = fetch_last & (extra_fetch_done | ~line_skipped);",
     "    wire fetch_cutoff = fetch_last;"),

    ("mut6", "the attribute pattern read from shift_reg[15], the colour half",
     "                grp <= {d_attr ? shift_reg[7] : shift_reg[15], shift_reg[14:12]};",
     "                grp <= {shift_reg[15], shift_reg[14:12]};"),

    ("mut7", "the depth and range clamps on pixel_skip dropped",
     "    wire [3:0] skip_fine = d_attr ? 4'd0 :\n"
     "                           {pixel_skip[3],\n"
     "                            pixel_skip[2],\n"
     "                            pixel_skip[1] & ~d_4bpp & ~clamp_2bpp,\n"
     "                            pixel_skip[0] & d_1bpp & ~clamp_1bpp};",
     "    wire [3:0] skip_fine = d_attr ? 4'd0 : pixel_skip;"),

    ("mut8", "the palette write pulse two clocks wide instead of one",
     "            nCLUT_WE <= ~wr1;",
     "            nCLUT_WE <= ~(wr1 | wr2);"),

    # 9: THE OLD SEQUENCE.  IDX at clocks 2..4 and the pulse at clock 3, which
    # is where this package's first draft had it.  pixel_tb.v catches it on the
    # clock numbers alone; clut_pair_tb.v catches the real defect (the data
    # changes on the very edge /WE rises), and OLD_PULSE below is its control.
    ("mut9", "the write sequence one clock late (the pre-2026-09-20 draft)",
     "    wire clut_seq = set_cdata | wr1 | wr2;",
     "    wire clut_seq = wr1 | wr2;"),

    ("mut10", "the index does not advance after a write",
     "            else if (wr2)\n"
     "            begin\n"
     "                // The post-increment: the edge that ends the pulse clock.  IDX\n"
     "                // is registered from the old value on this same edge, so the\n"
     "                // hold clock keeps the written address (header).\n"
     "                clut_index <= clut_index + 4'd1;\n"
     "            end\n",
     ""),

    # The increment one clock EARLY: clut_index moves at the end of clock 1, so
    # IDX — registered from it at the end of clock 2 — shows the NEXT entry on
    # the hold clock.  The RAM's address would move while it is still holding.
    ("mut11", "the index advances on wr1, one clock early, moving IDX on the hold clock",
     "            else if (wr2)\n",
     "            else if (wr1)\n"),
]

# The same mistake again, but as a WHOLE-FILE mutant, because clut_pair_tb.v
# instantiates the merged top level and not just the decoder.  The pulse is put
# back one clock late exactly as the first draft had it: a third flip-flop, the
# address window on wr1|wr2|wr3 and the pulse on ~wr2.
OLD_PULSE = [
    ("    reg wr1, wr2;\n",
     "    reg wr1, wr2, wr3;\n"),
    ("    wire clut_seq = set_cdata | wr1 | wr2;",
     "    wire clut_seq = wr1 | wr2 | wr3;"),
    ("            wr1 <= 1'b0;\n            wr2 <= 1'b0;\n            nCLUT_WE <= 1'b1;",
     "            wr1 <= 1'b0;\n            wr2 <= 1'b0;\n            wr3 <= 1'b0;\n            nCLUT_WE <= 1'b1;"),
    ("            wr1 <= set_cdata;\n            wr2 <= wr1;\n\n            nCLUT_WE <= ~wr1;",
     "            wr1 <= set_cdata;\n            wr2 <= wr1;\n            wr3 <= wr2;\n\n            nCLUT_WE <= ~wr2;"),
]

os.makedirs(OUT, exist_ok=True)

for name, why, old, new in MUTANTS:
    if pixel.count(old) != 1:
        sys.exit(f"{name}: anchor not unique ({pixel.count(old)}) in pixel.v's "
                 f"Pixel module:\n{old}")

    body = pixel.replace(old, new)
    head = (f"// {name}.v — NEGATIVE CONTROL generated by mutate.py from pixel.v:\n"
            f"// {why}.\n"
            f"// pixel_tb.v MUST FAIL on this file.  Not a design; do not edit.\n")
    open(os.path.join(OUT, f"{name}.v"), "w").write(head + body)

# The whole file, minus the `include (the Makefile passes timing.v explicitly)
# and minus the //PIN block (a mutant is never fitted).
full = src[:src.index('//PIN: CHIP "pixel"')]
full = full.replace('`include "timing.v"', '// (timing.v is passed on the command line)')

for old, new in OLD_PULSE:
    if full.count(old) != 1:
        sys.exit(f"old_pulse: anchor not unique ({full.count(old)}):\n{old}")
    full = full.replace(old, new)

open(os.path.join(OUT, "old_pulse.v"), "w").write(
    "// old_pulse.v — NEGATIVE CONTROL generated by mutate.py from pixel.v:\n"
    "// the palette write pulse one clock LATE (IDX on clocks 2..4, /WE low on\n"
    "// clock 3), which is what this package shipped in its first draft.  The\n"
    "// data then changes on the same edge /WE rises, so the RAM gets NO data\n"
    "// hold.  clut_pair_tb.v MUST FAIL on this file.  Not a design; do not edit.\n"
    + full)

print("wrote " + " ".join(f"mutants/{m[0]}.v" for m in MUTANTS) + " mutants/old_pulse.v")
