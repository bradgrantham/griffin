#!/usr/bin/env python3
"""genpins.py -- Part 2: re-pin a rung under a released-pin regime.

    python3 genpins.py r2 pmin      -> r2_pmin.v

The logic is the rung's, byte for byte; only the //PIN: block changes.
run_fitter.sh writes the .pin from that block, so the fit sees exactly it.

Regimes (block = ATF1508 LAB; PLCC84 I/O pins per block from r0.fit):
  A 4 5 6 8 9 10 11 12   B 15-18 20-22   C 24 25 27-31   D 33-37 39-41
  E 44-46 48-52          F 54-58 60 61   G 63-65 67-70   H 73-77 79-81

  pmin    RGB_OUT[11:0] lines deleted, every other pin frozen (rev 1).
  p444    E keeps RGB_OUT 7 8 5 4; RGB_OUT 3 0 1 2 SWAP with VIDCMD_Q 3 2 4 6
          (block D pins 39 40 41 37); F keeps its four.  4/4/4 over D E F.
  p444g   as p444 but the four go to block G, swapping with VIDCMD_Q 10 11 8 13
          (pins 63 64 65 67).  4/4/4 over E F G.
  p3333   3 per block over D E F G: E keeps 7 8 5; RGB_OUT 4 3 0 swap with
          VIDCMD_Q 3 2 4 (D 39 40 41); RGB_OUT 1 2 swap with VIDCMD_Q 10 11
          (G 63 64); RGB_OUT 6 swaps with VIDCMD_Q 8 (G 65); F keeps 10 11 9.
  p444i   p444 plus RGB_IN spread: RGB_IN 3 0 1 2 (the bits whose RGB_OUT
          moved to D) swap with the VIDCMD_Q now on E pins 49-52.  (RGB_IN
          are inputs; this tests whether input placement matters at all.)
  pfree   no //PIN: lines at all (empty .pin = free placement under keep).
"""

import re
import sys
import pathlib

SWAPS = {
    "p444":  [("RGB_OUT_3", "VIDCMD_Q_3"), ("RGB_OUT_0", "VIDCMD_Q_2"),
              ("RGB_OUT_1", "VIDCMD_Q_4"), ("RGB_OUT_2", "VIDCMD_Q_6")],
    "p444g": [("RGB_OUT_3", "VIDCMD_Q_10"), ("RGB_OUT_0", "VIDCMD_Q_11"),
              ("RGB_OUT_1", "VIDCMD_Q_8"), ("RGB_OUT_2", "VIDCMD_Q_13")],
    "p3333": [("RGB_OUT_4", "VIDCMD_Q_3"), ("RGB_OUT_3", "VIDCMD_Q_2"),
              ("RGB_OUT_0", "VIDCMD_Q_4"), ("RGB_OUT_1", "VIDCMD_Q_10"),
              ("RGB_OUT_2", "VIDCMD_Q_11"), ("RGB_OUT_6", "VIDCMD_Q_8")],
}
# p444i: p444, then RGB_IN 3 0 1 2 swap with the VIDCMD_Q now on 49 50 51 52
SWAPS["p444i"] = SWAPS["p444"] + [("RGB_IN_3", "VIDCMD_Q_3"), ("RGB_IN_0", "VIDCMD_Q_2"),
                                  ("RGB_IN_1", "VIDCMD_Q_4"), ("RGB_IN_2", "VIDCMD_Q_6")]

rung, regime = sys.argv[1], sys.argv[2]
text = pathlib.Path(f"{rung}.v").read_text()
lines = text.split("\n")
pin_re = re.compile(r"^//PIN: (\w+)\s*: (\d+)$")
pins = {}
for ln in lines:
    m = pin_re.match(ln)
    if m:
        pins[m.group(1)] = int(m.group(2))

if regime == "pfree":
    new = {}
elif regime == "pmin":
    new = {k: v for k, v in pins.items() if not k.startswith("RGB_OUT_")}
else:
    new = dict(pins)
    for a, b in SWAPS[regime]:
        new[a], new[b] = new[b], new[a]

out = []
done = False
for ln in lines:
    if ln.startswith("//PIN:"):
        if not done:
            if regime != "pfree":
                out.append('//PIN: CHIP "%s_%s" ASSIGNED TO AN PLCC84' % (rung, regime))
                for k, v in new.items():
                    out.append("//PIN: %-17s : %d" % (k, v))
            done = True
        continue
    out.append(ln)
pathlib.Path(f"{rung}_{regime}.v").write_text("\n".join(out))
moved = sorted(k for k in pins if k in new and new[k] != pins[k])
print(f"{rung}_{regime}.v: {len(new)} pins assigned, moved: {' '.join(moved) or '-'}")
