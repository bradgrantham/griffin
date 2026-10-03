#!/usr/bin/env python3
"""mutate.py -- single-mistake mutants of compositor.v for `make compositor-mutants`.

Each mutant is compositor.v with ONE deliberate mistake, written to
mutants/<name>.v (not checked in).  compositor_tb.v must FAIL on every one of
them: a bench that cannot fail is not a spec.
"""
import pathlib

HERE = pathlib.Path(__file__).parent
SRC = (HERE / "compositor.v").read_text()

MUTANTS = {
    # the literal does not hold the bank: the next word overwrites its colour
    "lit_no_hold": (
        "    wire buffer_frees = (~have_staged | consume_now) & ~mask_holds & ~load_literal;\n",
        "    wire buffer_frees = (~have_staged | consume_now) & ~mask_holds;\n"),
    # KEEP feeds the wrong source (passthrough instead of RGB_OUT)
    "keep_wrong_source": (
        "                                       RGB_OUT;              // SRC_KEEP\n",
        "                                       RGB_IN;               // SRC_KEEP\n"),
    # the streaming flag is sticky across runs (never cleared by a later run)
    "stream_sticky": (
        "                streaming  <= w_streaming;\n",
        "                streaming  <= streaming | w_streaming;\n"),
    # HOLD decoded on bit 10 instead of bit 11
    "hold_wrong_bit": (
        "    wire [1:0]  w_run_src   = w_src | {~staged_word[11], ~staged_word[11]};\n",
        "    wire [1:0]  w_run_src   = w_src | {~staged_word[10], ~staged_word[10]};\n"),
    # a nine-bit run counter
    "counter_9bit": (
        "    wire [9:0]  w_load      = staged_word[9:0];\n",
        "    wire [9:0]  w_load      = {1'b1, staged_word[8:0]};\n"),
}

out = HERE / "mutants"
out.mkdir(exist_ok=True)
for name, (old, new) in MUTANTS.items():
    if SRC.count(old) != 1:
        raise SystemExit(f"{name}: anchor matched {SRC.count(old)} times")
    (out / f"{name}.v").write_text(SRC.replace(old, new))
    print("wrote", f"mutants/{name}.v")
