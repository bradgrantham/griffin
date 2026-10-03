# dmabench — what a screen costs the 68000 in ENGINE DMA

> **Update 2026-09-19 (later the same day): both instrument faults are fixed in
> `emulator/emulator.cpp`.**  `service_video()` charges `check_timer()`'s return
> value again, and the SMOOTH pay-down no longer caps at a constant: it holds
> the unpaid debt equal to the engine's share of the cycles left in the line, so
> a line's charge is retired exactly at the line's end for any load.  The
> **stock** emulator with **no** environment variables now reproduces the
> `GRIFFIN_DMA_STALL_STEP=64` column below unit-for-unit, so ignore the
> "patched emulator" and "STEP=64" instructions in the recipes — plain
> `emulator/build/emulator` is correct and `emu/` is obsolete.  Knobs are
> documented in `emulator/CLAUDE.md`; `check-dma-stall.sh` is the regression
> check.

**2026-09-19.  Every number here is the EMULATOR'S COST MODEL, not silicon.**
Nothing was measured on a bench.  What the emulator charges is
`super-engine/descriptor.h`'s per-descriptor bus model, and the agreement
reported below is agreement between a measurement of that model and a
closed-form calculation from the same model — a self-consistency check on the
instrument, not evidence about hardware.

## What was asked and what is here

How much CPU throughput does a 68000 program lose to video DMA for different
screen types, and in particular what does an 80-column text console cost when it
is built purely from COMPOSITOR VIDCMD MASK records (`vidcmd-spec.md` §4.5)
instead of from a 640x480 1bpp PIXEL bitmap?

* `dmabench.cpp` — the app.  One bare-metal binary that, for each configuration,
  builds the display list once, arms it, and runs a fixed CPU-bound kernel for
  exactly F fields counted off the GLUE vblank latch, re-arming ENGINE once per
  observed vblank, and reports the number of kernel units completed.
* `emu/build-patched-emulator.sh` — a patched emulator, needed because the
  shipped one does not charge the stall at all.  See "The instrument" below.
* `shot-C1.png` .. `shot-C4.png` — screenshots proving each list is legal and
  renders what it claims to.  C1 and C2 show the same 80x60 text screen by the
  two different routes, which is the point.

## The configurations

| tag | screen | per line |
|---|---|---|
| C0  | ENGINE never armed — the no-DMA reference | — |
| C0B | blank screen from the smallest legal list: one VIDCMD `HOLD(1)` in the vblank preamble (KEEP: black at every line start; was `RUN_COLOR(black,1)` until 2026-10-03, same one-slot timing) plus one `wait_hblank`/`stop_after` pacer, 2 descriptors total | — |
| C1  | 640x480 1bpp PIXEL console, the shape `firmware/rom.cpp:856 dl_build_console_list()` emits | 40 PIXELS words as 2x20-word descriptors, first waiting |
| C2  | pure-VIDCMD MASK text, §4.5's 82-word line | 40 MASK records (80 words) + 2 colour SETs, 3 descriptors (32/32/18) |
| C3  | 2bpp full rate, one palette entry reloaded per line | 80 PIXELS words (3 desc) + 6 VIDCMD words (1 desc) |
| C4  | half-rate 1bpp playfield under a lot of VIDCMD (≈ the Smurf load) | 20 PIXELS words (1 desc) + 60 VIDCMD words (2 desc) |

**2026-09-20, the indexed PIXEL.**  `pal_fg`/`pal_bg`/`ham_held` are gone; every
playfield pixel is a palette index and a colour arrives as the three-word palette
entry of `vidcmd-spec.md` §3.2.  Every configuration's **words per line is
unchanged**, deliberately, so the analytic column below stays comparable:

* C1 and C4 moved their palette entries and their mode/base SETs into a
  once-per-FRAME vblank preamble (one extra descriptor, +6 and +7 words a frame).
* C2's two line-0 SETs became `pix_base` and `pix_mode`; still two words.
* C3 keeps its six words a line, now a three-word palette entry for entry 3 plus
  the two COMPOSITOR colour SETs plus the 640-slot RUN — so it still reloads one
  colour every scanline, which is the thing C3 exists to price.
* C4 also gained a fix: it authored 61 words a line and streamed 60, so the
  trailing `RUN(passthrough,192)` was dropped on every line.  Authored and
  streamed are both 60 now.

The re-measurement at 40 fields is in the table below the 300-field one.

C0 answers "video fully off": ENGINE is simply never armed, which is legal —
the vblank latch comes from TIMING and does not depend on ENGINE, so the app's
field clock still runs.  C0B is there because "smallest legal list" is the more
useful floor: a whole blank frame costs **38 SYSCLK, 0.02 % of a field**.
COMPOSITOR holds the last RUN's source to the end of a line and a line that
receives no records keeps holding, so one one-slot RUN paints all 480 lines.

Every list also carries the 34-pacer vblank walk (lines 491..524) and a trailing
`wait_hblank`/`stop_after` pacer, as `firmware/rom.cpp` and `apps/dtest` do.
Those 35 wait-descriptors cost 22 SYSCLK each, 770 SYSCLK a frame.

## The instrument, and the bug in it

**`emulator/emulator.cpp` as shipped does not charge the per-scanline DMA stall
at all.**  `VideoState::check_timer()` (emulator.cpp:1234-1266) accumulates each
line's walker cost into a local and **returns** it; `service_video()`
(emulator.cpp:3993-4005) **discards that return value** and charges only
`engine_stall_pending`, which holds the arm-time (vblank-preamble) walk — 34
SYSCLK a frame for the console list.  `git log -L` on the region shows the
regression: before commit 76c8a9b ("Emulator: display-list pipeline via the
super-engine suite objects") the line read

```cpp
uint32_t dma_stall = video.check_timer(getClock(), ...);
```

and the rewrite that moved the pipeline onto the super-engine objects dropped the
assignment.  Measured consequence, stock emulator, 20 fields:

```
cfg   desc  words   SYSCLK   DMA/fr    units  vs C0
C0       0      0        0     0.0%      1513   100.0%
C1     996  19238    55510    23.8%      1512    99.9%     <-- should be ~76 %
```

`emu/build-patched-emulator.sh` restores the one line (`dma_stall +=
video.check_timer(...)`) in a copy under `apps/dmabench/emu/`, links it against
the SDL3 `emulator/build` already fetched, and changes nothing outside this
directory.  Everything below is that patched binary.

**Second instrument trap: `GRIFFIN_DMA_STALL_STEP` defaults to 4.**  The SMOOTH
model pays the accrued stall down at most STEP cycles per CPU step
(emulator.cpp:4086-4103), and `service_video()` runs once per instruction, so the
largest stall fraction it can ever charge is `STEP/(mean instruction cycles +
STEP)`.  With this kernel's ~7.6-cycle mean that saturates at **34.4 %**, and C2,
C3 and C4 all landed on exactly the same saturated 65.6 % under the default:

| cfg | analytic DMA share | STEP=4 (default) | STEP=64 | `GRIFFIN_DMA_STALL_LUMP` |
|---|---:|---:|---:|---:|
| C1 | 23.8 % | 76.1 % | 76.1 % | 76.1 % |
| C2 | 43.9 % | **65.6 %** | 55.9 % | 55.9 % |
| C3 | 48.4 % | **65.6 %** | 51.4 % | 51.4 % |
| C4 | 43.1 % | **65.6 %** | 56.7 % | 56.7 % |

STEP=64 and LUMP agree to the last unit, which is what says the debt fully
drains at 64 and the number is no longer clamped.  **STEP=64 is the primary
result.**  Anyone benchmarking a heavy list on the stock defaults is measuring
the clamp, not the list.

## The cost model, in two sentences

Each descriptor costs `arbitration(4) + assert(1) + four descriptor words(8) +
2 x payload words + release(1)`, and a `wait_hblank` descriptor pays the
arbitration and assert twice because it hands the bus back at the fetch and
re-arbitrates after the HBLANK edge — `super-engine/descriptor.h:291-317`,
charged by `EngineWalker` at `super-engine/interpret.cpp:112-143` (fetch and
payload) and `:218-248` (the release after the edge).  The emulator lumps a whole
line's worth of that at the line's HBLANK edge (`emulator.cpp:4018-4020`, and the
"KNOWN SIMPLIFICATION" note at `emulator.cpp:1290-1297`), then either pays it
down a few cycles per instruction (SMOOTH, the default) or injects it as one
clock jump (LUMP); vblank and blank lines are charged exactly like active ones,
because a pacer is a descriptor like any other (22 SYSCLK), and there is no VIDEO
register file left to charge for MMIO.

Per frame: 525 scanlines x `LINE_SYSCLK` 445 = **233,625 SYSCLK**.

## Results — 300 fields per configuration, patched emulator, `GRIFFIN_DMA_STALL_STEP=64`

```
dmabench: 300 fields per configuration, kernel = 256-word RMW pass over 1024 bytes of RAM
dmabench: carve 0x3F8000 + 32768 bytes (4096 descriptors), frame = 233625 SYSCLK
cfg   desc  words   SYSCLK   DMA/fr    units  vs C0      ms
C0       0      0        0     0.0%     22696   100.0%   5010
C0B      2      2       38     0.0%     22681    99.9%   5010
C1     996  19238    55510    23.8%     17275    76.1%   5010
C2    1475  39397   102534    43.9%     12691    55.9%   5010
C3    1955  41316   113092    48.4%     11662    51.4%   5010
C4    1475  38436   100612    43.1%     12879    56.7%   5000
```

Measured against the closed form computed from words/line, descriptors/line, 2
SYSCLK/word, 445 SYSCLK/line and 525 lines:

| cfg | SYSCLK/frame | analytic DMA share | analytic CPU left | **measured CPU left** | measured implied DMA | error (pp) |
|---|---:|---:|---:|---:|---:|---:|
| C0  | 0       | 0.00 %  | 100.00 % | 100.0 % | 0.0 %  | — |
| C0B | 38      | 0.02 %  |  99.98 % |  99.9 % | 0.1 %  | 0.08 |
| C1  | 55,510  | 23.76 % |  76.24 % |  76.1 % | 23.9 % | 0.14 |
| C2  | 102,534 | 43.89 % |  56.11 % |  55.9 % | 44.1 % | 0.21 |
| C3  | 113,092 | 48.41 % |  51.59 % |  51.4 % | 48.6 % | 0.19 |
| C4  | 100,612 | 43.07 % |  56.93 % |  56.7 % | 43.3 % | 0.23 |

### Re-measured after the indexed PIXEL, 40 fields, stock emulator (2026-09-20)

```
cfg   desc  words   SYSCLK   DMA/fr    units  vs C0      ms
C0       0      0        0     0.0%      3026   100.0%    670
C0B      2      2       38     0.0%      3024    99.9%    670
C1     996  19244    55522    23.8%      2303    76.1%    670
C2    1475  39397   102534    43.9%      1693    55.9%    660
C3    1956  41329   113132    48.4%      1555    51.4%    670
C4    1476  38443   100640    43.1%      1717    56.7%    670
```

Every ratio is identical to the 300-field run above and every analytic share is
identical to two decimal places.  What moved is only the frame preambles: C1
+6 words, C3 +1 descriptor and +13 words, C4 +1 descriptor and +7 words, all of
it once a frame rather than once a line.  `check-dma-stall.sh 40` passes on C1
(76.1 %) and C2 (55.9 %).

Every row agrees within a quarter of a percentage point — well under the "few
percent" threshold, so the measurement and the closed form are the same
statement and either can be trusted **about the model**.  The residual is
explained by the firmware's 100 Hz systick ISR, which runs in all six
configurations and eats a little of every one of them, and by the single
unfinished kernel unit at the end of each window.

The `ms` column is the firmware's 100 Hz tick read either side of the timed
region: 5.00-5.01 s for 300 fields in every configuration, i.e. 59.94 Hz.  That
is the cross-check that the field clock really is the raster and does not itself
slow down with the bus, which is the whole basis for comparing unit counts.

### The headline

**A MASK text console leaves the CPU 73.5 % of the throughput a 1bpp console
leaves it** (measured; 73.6 % from the closed form).  Put the other way: the
pure-VIDCMD 80-column console costs **1.85x** the bus of the 1bpp bitmap
console — 102,534 SYSCLK a frame against 55,510 — and takes the CPU's share of
its own machine from 76 % down to 56 %.

### Where C2's cost goes

82 words/line is 164 SYSCLK of payload, but the line costs 212 because
`DESC_MAX_COUNT` is 32, so 82 words needs three descriptors and each costs 14 or
20 SYSCLK of arbitration and descriptor fetch before it moves a word.  **23 % of
C2's DMA is descriptor overhead**, against 30 % for C1 (two 20-word descriptors
for 40 words).  A larger `DESC_MAX_COUNT` would be worth roughly 20 % of the
pure-VIDCMD screen's cost; that is an ENGINE encoding question, not a list
question.

C4 is the interesting one for the ports work: 20 PIXELS words and 60 VIDCMD
words costs 43 %, essentially the same as C2's 82 pure-VIDCMD words, because
what is expensive is words and descriptors, not which FIFO they land in.

## The firmware console today

**It is the 1bpp PIXEL kind, not VIDCMD.**  `firmware/rom.cpp:856
dl_build_console_list()` emits exactly C1: a three-word VIDCMD preamble
(`SET pix_pal_fg`, `SET pix_pal_bg`, `RUN(passthrough,1)`) in one non-waiting
descriptor, 34 pacers, then 480 lines of `dl_emit_pixel_line()` — 40 PIXELS
words per line as two 20-word descriptors, the first waiting — and a trailing
`stop_after` pacer.  996 descriptors, 3 VIDCMD words a frame.  The emulator's own
boot log confirms it: `ENGINE: list at 0x3F6000, 996 descriptors (7968 bytes),
3 VIDCMD words`.  Text reaches the screen through
`firmware/textport.cpp`, which rasterises glyphs into the linear 1bpp
framebuffer at 0x3857F0 (480 lines x 80 bytes) on every state change, and scrolls
by `memmove` of both the char/attr buffer and the pixel rows.

To make it the C2 kind, roughly:

* **The framebuffer and the rasteriser go.**  `Textport` keeps its char/attr
  buffer (that stays the model) but `put_glyph`, `clear_scanlines_` and the
  scroll's pixel `memmove` stop writing pixels; instead the VIDCMD word array
  becomes the render target.
* **A per-line MASK builder replaces it.**  For each of the 480 scanlines, 40
  two-word MASK records built from the char row's glyph bits, plus the per-line
  colour SETs.  Cells must be 8 pixels wide with column 0 blank so each record's
  implicit pixel 0 (forced to `cmp_color0`) lands on an inter-character gap —
  i.e. the console's font would change from the current 8x16/8x8/6x10 set to a
  5x7-in-8 layout, or the record geometry would have to change with it.
* **Storage grows and moves.**  80 KB of VIDCMD words against 38 KB of
  framebuffer, and the descriptor table goes from 996 descriptors (7,968 bytes)
  to 1,475 (11,800) — the console list currently has to stay below
  `GRIFFIN_APP_DESC_BASE`, so that budget and the `static_assert`s at
  `rom.cpp:842-854` would need revisiting.
* **The scroll gets cheaper, not dearer.**  Scrolling a MASK console is
  re-pointing 8 lines' worth of descriptor source addresses, or rebuilding the
  rows that moved; it is not a 38 KB `memmove`.
* **And the CPU pays 20 points of throughput for it** — the number this
  benchmark exists to produce.

Two things would also have to be settled that the benchmark deliberately avoids:
the third colour (dibit 00 = passthrough resolving to `pix_pal_bg` with the
PIXELS FIFO never written) is flagged in `vidcmd-spec.md` §4.5 as model
behaviour **not verified against hardware**, so a console that wanted the current
attribute set could not lean on it yet; and a MASK console has no inverse-video
attribute short of a SET pair per run of cells, which §4.5's last row prices as
the thing that actually runs out.

## Reproducing

```sh
cd apps/dmabench
make cf                       # dmabench.bin + dmabench-cf.img
./emu/build-patched-emulator.sh
export DYLD_LIBRARY_PATH=$(cd ../../emulator/build/_deps/sdl3-build && pwd)

printf 'DMABENCH 300\r' > cmds-300.txt
GRIFFIN_DMA_STALL_STEP=64 ./emu/emulator-patched --headless --no-throttle \
    --cf dmabench-cf.img --console-in cmds-300.txt \
    --console-out console-300-step64.txt \
    --run-cycles 1200000000 ../../firmware/rom.bin

printf 'DMABENCH shot 3\r' > cmds-shot3.txt      # index 3 = C2
./emu/emulator-patched --headless --no-throttle --cf dmabench-cf.img \
    --console-in cmds-shot3.txt --console-out console-shot3.txt \
    --screenshot shot.bmp --run-cycles 450000000 ../../firmware/rom.bin
sips -s format png shot.bmp --out shot-C2.png
```

`shot N` arms configuration N and holds it forever; `--screenshot` is written at
exit, so the app must still own the display when `--run-cycles` ends the run.
C2's list takes ~200 M cycles to author on the target (19,200 MASK records built
one at a time through `vidcmd_mask_from_planes`), which is why its screenshot run
needs more cycles than the others — that is authoring cost, entirely outside the
timed region, and a real console would build records incrementally as glyphs
change.

`emu/emulator_patched.cpp` and `emu/emulator-patched` are build outputs of the
script and are not sources; nothing under `emulator/` was touched.

## Not done

* No DUART-timer measurement.  The firmware owns the DUART for the console and
  reprogramming its counter under a live console is a way to lose the transcript;
  the firmware's 100 Hz systick was used as the wall-clock cross-check instead
  and it corroborated the field count in every run (5.00-5.01 s / 300 fields).
* No hardware measurement of any kind.
* `--selftest-displaylist`-style validation of C2/C3/C4 as lists was not run;
  their legality rests on the authored slot arithmetic (asserted in the source)
  and on the four screenshots.
