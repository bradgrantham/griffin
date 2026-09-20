# Indexed PIXEL + palette RAM — spec draft for review

Status: DRAFT 2026-09-20.  Nothing here is law until it is folded into
`vidcmd-spec.md` together with the RTL, testbench, model, suite and emulator
(CLAUDE.md, video-chain discipline).  Evidence and fit ladders:
`cpld/experiments/indexed-pixel/RESULTS.md`, `image-tools/clut-experiment/`,
`apps/dmabench/`, `griffin.log` 2026-09-18..20.  The pre-change state is tagged
`rev1-video`; rev-1 boards cannot take this bitfile (shared PIXELS Q bus, no
palette RAM).

## 0. Decisions taken (Brad, 2026-09-19/20)

* PIXEL becomes index-only; micro-HAM and `pal_fg`/`pal_bg`/`ham_held` retire.
* Palette = 16 x 12 bits in three 16x4 separate-I/O bipolar RAMs, 74F219
  pinout (N74F219AN preferred; Am27S07 same footprint, ~300 mA for three).
  Non-inverting parts only.
* Write form "W2": RAM data inputs sit on COMPOSITOR's `set_pix_value[11:0]`;
  PIXEL supplies the address and the write pulse.
* PIXELS FIFO read as a ganged 16-bit port with one read strobe.
* COMPOSITOR RTL, pins and VIDCMD word formats do not change.
* Mid-line mode changes stay undefined and unspecified.
* Skip limits below are accepted as recorded limitations.
* A `clut_data` SET is followed by a same-value `SET target 7` filler; a table
  entry costs 3 words (accepted 2026-09-20).
* TIMING is folded into the PIXEL chip (production fit 123/128 LC, 43 I/O, pass 1).

Deferred, not part of this change: the COMPOSITOR `PIX_BIT` bypass and 74F374 hold registers (only needed
if the RAM misses the read budget in §6).

## 1. Architecture

```
ENGINE ─► PIXELS FIFO (2x7200, 16-bit ganged) ─► PIXEL ─ IDX[3:0] ─► 3x 74F219 ─ RGB[11:0] ─► COMPOSITOR ─► DAC
       └► VIDCMD FIFO ───────────────────────────────────────────────────────────────────────┘
          COMPOSITOR ─ set_pix_value[11:0] ─► 74F219 data inputs;  [3:0] also ─► PIXEL
          COMPOSITOR ─ set_pix_target/valid/commit ─► PIXEL
          PIXEL ─ nCLUT_WE ─► 74F219 /WE (all three);  /CS tied low
```

Every playfield pixel in every mode goes through the palette.  COMPOSITOR's own
colours (`cmp_color0/1`, RUN_COLOR, MASK) stay direct 12-bit and never touch it.

## 2. PIXELS stream

Words are consumed MSB first, bit 15 down; existing records remain valid bit
for bit (old first byte = `PIXELS_Q[15:8]`, `pixel.v:75-76`).

`SET pix_mode`: `mode[1:0]` = depth, `mode[2]` = half rate (each group held two
pixel clocks).  `value[3]` is reserved for the COMPOSITOR bypass and must be 0.

| depth | `mode[1:0]` | index | words/line full | half |
|---|---|---|---|---|
| 1bpp | 00 | `{pix_base[3:1], bit}` | 40 | 20 |
| 2bpp | 01 | `{pix_base[3:2], dibit}` | 80 | 40 |
| 4bpp | 10 | nibble | UNDEFINED (desyncs the frame) | 80 |
| attribute | 11 | `bit ? fg : bg` | 80 | 40 |

Attribute word: `{fg[3:0], bg[3:0], pattern[7:0]}`, one word per 8-group cell,
pattern MSB first.  With the TMS9918 palette loaded it is the Graphics II
colour byte followed by the pattern byte.  At full rate it is an 80-column
cell-coloured text/tile mode; at half rate a 320-wide tile mode.

Mode changes belong in blanking.  A line's descriptors supply that line's word
count, so mode may differ line to line.

### pixel_skip

Four bits, counted in stream bits: pixel 0 is stream bit `s` of the line's
first word.  Coarser scrolling moves the descriptor's source pointer by words.

* Depth clamp: 1bpp `s`; 2bpp `s & ~1`; 4bpp `s & ~3`; attribute forced to 0.
* Top-of-range cap (recorded limitation): 1bpp 15 -> 14 at both rates; 2bpp
  full rate 14, 15 -> 12; 2bpp half rate 15 -> 14; 4bpp reaches 0, 4, 8, 12.
* A line with non-zero skip consumes ONE EXTRA WORD (record + 1) and its tail
  shows real data.  The list author must supply it.
* Skip is sampled at the line's preload: set it in blanking, before the line.
  A mid-line SET affects the next line only.

## 3. SET targets

| target | meaning |
|---|---|
| 0 / 1 | `cmp_color1` / `cmp_color0` (COMPOSITOR, unchanged, swapped numbering unchanged) |
| 2 | `clut_index[3:0]` |
| 3 | `clut_data[11:0]` — written to the palette at `clut_index` |
| 4 | `pix_base[3:0]` |
| 5 | `pix_mode[2:0]` (+ reserved bit 3) |
| 6 | `pixel_skip[3:0]` |
| 7 | spare; PIXEL ignores it (used as the write filler below) |

Reset (/RS at vsync): `pix_mode`, `pix_base`, `pixel_skip`, `clut_index` = 0.
**The palette RAM is never reset** and powers up undefined; software loads what
it uses.  Contents persist across frames.  (Model and emulator: random at
power-up.)

## 4. Palette write

Sequence, in pixel clocks after the `clut_data` commit: clock 1 `IDX` =
`clut_index`; clock 2 `nCLUT_WE` low; clock 3 `nCLUT_WE` high with address and
data still stable; clock 4 `IDX` returns to the pixel index.  The one-clock
pulse is centred in the three-clock address window, so both the address and the
data are stable for a full clock before it falls and a full clock after it
rises.  RAM outputs are high-Z while `/WE` is low and need the part's
write-recovery time afterwards.

Rules for the list author:

1. **Where.**  A write disturbs the pixel bus for clocks 1..4 after the commit
   (1..3 measured, plus one clock of guard for write recovery).  Legal only in
   blanking, or where those pixel slots are covered by a non-passthrough RUN /
   opaque MASK.  Clocks 1..3 are the entry's own three words; clock 4 is the
   first clock of the word after the filler, so that word must not be the RUN
   that resumes passthrough.
2. **Hold.**  `set_pix_value` must carry the payload across clocks 1, 2 and 3.
   COMPOSITOR re-registers the bus from every staged word and stages one every
   two clocks, so the `clut_data` SET's own value covers clocks 0 and 1 only
   and the word after it covers 2 and 3: a `clut_data` SET is therefore
   followed by one filler word carrying the SAME 12-bit value, `SET target 7`.
   It must be target 7 — any other target either writes a register with the
   colour payload or starts a second write — and it is necessary, not
   decorative.  (Pinned by `cpld/pixel/clut_pair_tb.v`, which also carries the
   negative control for the one-clock-later sequence this replaced: that one
   gave the RAM zero data hold, met only by inter-chip skew.)
3. Cost per entry: `SET clut_index`, `SET clut_data`, filler = 3 words.  A
   26-word hblank packet reloads 8 entries per line with room for the mode /
   base / skip SETs.  Photographs need 2-5 per line (`clut-experiment`).
   Consecutive entries could drop the index SET if `clut_index`
   post-incremented on each write (2 words/entry, 13 per line) — OPEN, costs
   LCs, not needed for Smurf.

## 5. Signals that change

| signal | path | note |
|---|---|---|
| `PIXELS_Q[15:0]` | FIFO pair -> PIXEL | each 7200 on its own eight pins; shared Q bus retired |
| `nPIXELS_RE` | PIXEL -> both 7200 | registered, one clock low per word, >= 8 clocks apart; `_EVEN/_ODD` retired |
| `IDX[3:0]` | PIXEL -> RAM address | replaces `RGB[11:0]` out of PIXEL |
| `nCLUT_WE` | PIXEL -> RAM /WE | |
| `RGB[11:0]` | RAM outputs -> COMPOSITOR `RGB_IN` | |
| `set_pix_value[11:0]` | COMPOSITOR -> RAM data in; `[3:0]` also -> PIXEL | PIXEL drops `[11:4]` |

PIXEL + TIMING in one ATF1508AS: production fit 123/128 LC, 94 FF, 384 PT, 43 I/O, pass 1 (`cpld/pixel/LANDING-NOTES.md`; experiment `R8w` was 122); standalone PIXEL 85 LC, 38 I/O (`R5w`).  The TIMING chip, its JTAG position and its chip-to-chip nets retire; its external outputs move to the merged chip.  COMPOSITOR unchanged.

## 6. Timing to pin before the board is routed (datasheet tables, not estimates)

* Read path per pixel clock, T = 39.72 ns: PIXEL tCO + flight + RAM address
  access + flight <= T - COMPOSITOR tSU.  With the ATF1508AS-10 numbers already
  pinned for the VIDCMD FIFO port (tCO 8.0, ~2 flight, T - tSU = 32.7) the RAM
  gets **22.7 ns max address access** over 0-70 C.  Fairchild 74F219 is 27 ns
  (fails); N74F219AN and Am27S07-15 are believed to pass — read the table of the
  part actually bought.  Confirm PIXEL's speed grade.
* Write: /WE pulse width, address setup/hold, data setup/hold against the
  one-clock pulse and the §4 sequence; write recovery against rule 1's window.
* PIXELS read port: same budget form as the VIDCMD port (tA 15 ns); sixteen
  inputs sampled on one edge.
* K constants (`vidcmd-spec.md` §6) re-measured by the testbench; a
  combinational RAM should leave them unchanged.

## 7. Retired

micro-HAM (RTL, `image-tools/microham*`, suite cases, the smurf-demo `.ham`
backdrops), `pal_fg`/`pal_bg`/`ham_held`, 2bpp-indexed's fixed four colours,
the byte-interleaved PIXELS read and the extra-byte skip discard (with its
adjacent-clock bus turnover and the odd-byte-count mis-pairing bug).

## 8. Landing checklist

1. Fold this into `vidcmd-spec.md`; `griffin.yml` SET targets, mode field,
   words-per-line constants, "Stream FIFO parts", palette interface; root `make`.
2. `cpld/pixel/pixel.v` from `r5w.v` with `timing.v` instantiated in the same top level (`r8w`), new pin block, Makefile targets, production fit logged; `timing_tb.v` still passes against the merged top.
3. `pixel_tb.v` from `idxw_tb.v` (hand-derived expectations, mutants); NEW
   chip-pair bench COMPOSITOR + PIXEL + RAM model pinning §4 rule 2.
4. `super-engine/render.{h,cpp}`: PixelUnit (index out, modes, skip law, words
   consumed), palette unit (random power-up, illegal-window writes flagged),
   `descriptor.h` constants.
5. Suite: retire HAM / old indexed cases; new cases for every mode, per-line
   reload, skipped line with record + 1, one illegal write.
6. Emulator rebuild; firmware boots headless; `apps/dmabench/check-dma-stall.sh`.
7. Firmware console list (two palette entries per frame, base, mode); apps;
   `clut-experiment` encoder promoted to a tool.
8. `griffin-video-datasheet.html` (status back to PRELIMINARY), `griffin.md`,
   dated adoption entry in `griffin.log`.
9. Rev-2 board: eight Q traces, three RAMs, `IDX`, `nCLUT_WE`; TIMING socket removed, JTAG chain GLUE -> PORTS -> ENGINE -> PIXEL -> COMPOSITOR, DUART OP4-7 mode pins to the merged chip.
