#!/usr/bin/env python3
"""mutate.py -- single-mistake mutants of r8b.v (form C) for lit_tb.v -DFORM_C.
Each MUST make the bench fail."""
import pathlib
src = pathlib.Path("r8b.v").read_text()
M = {
    # the literal selects colour1 but forgets to write it
    "m1": ("    wire write_color1 = (apply_set & (w_target == 3'd0)) | load_literal;\n",
           "    wire write_color1 = (apply_set & (w_target == 3'd0));\n"),
    # the literal writes colour1 but cur_src takes src 11 (aliases colour0)
    "m2": ("                cur_src    <= w_is_literal ? SRC_COLOR1 : w_src;\n",
           "                cur_src    <= w_src;\n"),
    # the literal also loads its colour bits as a count
    "m3": ("                            load_count ? w_load  :\n",
           "                            load_run   ? w_load  :\n"),
}
for name, (old, new) in M.items():
    assert src.count(old) == 1, name
    pathlib.Path(f"r8b_{name}.v").write_text(src.replace(old, new))
    print("wrote", f"r8b_{name}.v")
