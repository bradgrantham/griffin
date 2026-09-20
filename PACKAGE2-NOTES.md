# Package 2 — definitions, model, suite, emulator, firmware

Package 1 (commits 88a1e3c, f39178b) landed the RTL and the chip testbenches.
This package propagates that through the other four layers of the video-chain
discipline.  The contract is `indexed-pixel-spec-draft.md` (now folded into
`vidcmd-spec.md`) and `cpld/pixel/LANDING-NOTES.md`; **where this document and a
testbench disagree, the testbench wins** and the disagreement is recorded at the
bottom.

Nothing is committed by this work.  `griffin.log` and
`griffin-video-datasheet.html` are deliberately untouched (owner's).

## Layer 1 — DEFINITIONS (`griffin.yml`, `vidcmd-spec.md`)

`vidcmd-spec.md`

* New addendum at the top marking the indexed PIXEL / palette RAM / TIMING
  merge, and stating that §2, §3's SET table, §3.2, §5, §6, §7 and §8 were
  rewritten rather than annotated.  The 2026-08-26 "the mode register is a BIT
  FIELD" addendum is **deleted** with the modes it described.
* §1 diagram now carries the palette RAM and drops the TIMING row.
* §2 rewritten: index-only, ganged 16-bit read, the four depths + half rate,
  the attribute word, the measured fetch cadence table (T and clocks/word), the
  8-clock /RE floor, the `pixel_skip` law with both clamps and the caps, and the
  `record + (skip != 0)` words-per-line rule.
* §3's SET table renumbered (2 = `clut_index`, 3 = `clut_data`, 4 = `pix_base`,
  7 = the filler) and NEW **§3.2 The palette**: the three-word entry, the
  clock-by-clock write sequence, the hold rule (why target 7 and why it is
  necessary), the disturbed window 1..4, and the per-line / per-frame cost.
* §4.5's "third colour" is now "one palette entry" and its caveat gained the
  third open question (the RAM powers up undefined).
* §5 records the two destinations of `set_pix_value` (all twelve to the RAM,
  `[3:0]` to PIXEL) and that it is re-registered from every staged word — the
  mechanism the filler rule falls out of.
* §6 gained the unsettled palette read/write arithmetic; §7 PIXEL rewritten
  (index-only + the raster generator in the same chip); §8 signal table
  rewritten; §9 updated; §10 gained open items 9 (the part choice) and 10
  (`clut_index` post-increment) and item 8 was reworded.
* `indexed-pixel-spec-draft.md` keeps all its content and gained a four-line
  **FOLDED IN** header, as asked.

`griffin.yml`

* The `TIMING` peripheral entry is **deleted**; its raster contract and its
  `video_timing` block moved under `PIXEL`, which is now described as both the
  raster time base and the indexed unpacker.  Every other `TIMING's X` reference
  in the file became `PIXEL's X`.
  * **Generated-name change**: `TIMING_H_*` / `TIMING_V_*` / `TIMING_PIXEL_CLOCK_HZ`
    are now `PIXEL_H_*` / `PIXEL_V_*` / `PIXEL_PIXEL_CLOCK_HZ`.  Nothing in the
    tree consumed the old names (checked with grep over `*.v`, `*.cpp`, `*.h`,
    `*.vh`, `*.inc`, `*.py`) except the generated headers themselves.
  * `TIMING_SPLIT_LEAD` **kept** under its own name: it is still a fact (one
    registered strobe hop, now internal to PIXEL) and `timing.v`'s localparam
    `SPLIT_LEAD` is its other end.  Only the comment changed.
* PIXEL's contract now states: ganged 16-bit PIXELS port with one `/RE`; MSB
  first; index-only with the four index formations; mode as a depth enum + half
  rate + reserved bit 3; `PIXEL_SKIP` as a stream-bit start position with the
  depth clamp, the caps and the record+1 rule; the palette write sequence, the
  filler and the four disturbed pixel clocks; the reset values; and that the
  palette RAM is never reset.  COMPOSITOR's power-on-reset dependency on the
  fitter's `power_reset` flag is stated once, under PIXEL's `nRS` bullet (that is
  the net's home).
* New constants:
  `VIDCMD_SET_PIX_CLUT_INDEX` 2, `VIDCMD_SET_PIX_CLUT_DATA` 3,
  `VIDCMD_SET_PIX_BASE` 4, `VIDCMD_SET_FILLER` 7,
  `VIDCMD_WORDS_PER_CLUT_ENTRY` 3,
  `VIDCMD_PIX_MODE_{1BPP,2BPP,4BPP,ATTR,HALF_RATE}` = 0,1,2,3,0x4,
  `PIXELS_WORDS_PER_LINE_{1BPP,1BPP_HALF,2BPP,2BPP_HALF,4BPP_HALF,ATTR,ATTR_HALF}`
  = 40,20,80,40,80,80,40, `PIXELS_SKIP_EXTRA_WORDS` 1,
  `PIXELS_SKIP_MAX_{1BPP,2BPP,2BPP_HALF,4BPP_HALF}` = 14,12,14,12.
  Removed: `VIDCMD_SET_PIX_PAL_FG`, `VIDCMD_SET_PIX_PAL_BG`,
  `VIDCMD_SET_PIX_HAM_HELD`, `PIXELS_WORDS_PER_LINE_2BPP`'s micro-HAM comment.
* `interfaces:` — "Stream FIFO parts" rewritten for the ganged PIXELS read with
  one strobe; NEW "Palette RAM" entry (16x12 from three 16x4 separate-I/O parts,
  address and /WE from PIXEL, data in from COMPOSITOR's `set_pix_value`, outputs
  to `RGB_IN`, never reset, tAA is a selection criterion); NEW "JTAG chain"
  entry (GLUE -> PORTS -> ENGINE -> PIXEL -> COMPOSITOR, `{earlier}_TO_{later}_TD`,
  no TIMING TAP).  The VGA output-stage entry now takes its syncs from PIXEL.
* `issues:` — "Micro-HAM stream encoding is not captured in the YAML" is
  deleted; added the palette-RAM part/arithmetic issue and the `clut_index`
  post-increment issue.

Gate: `make` (codegen) and `make check-generated` (schema validate + staleness)
both pass.

### Numbers taken from the testbenches, and where they came from

| number | value | source |
|---|---|---|
| words/line, 1bpp | 40 / 20 | `pixel_tb.v` header MEASURED CONSTANTS + case A/F, re-run at 640 in case W |
| words/line, 2bpp | 80 / 40 | same, cases B / W |
| words/line, 4bpp | UNDEFINED / 80 | same, case C (full rate untested by design) |
| words/line, attribute | 80 / 40 | same, cases D / E / D2 / E2 |
| fetch trigger T | 14/15, 12/14, 8/12, 6/7 | `pixel.v` `fetch_due` + `pixel_tb.v` derivations part 1 |
| clocks per word | 16/32, 8/16, 4/8, 8/16 | same |
| /RE floor between reads | 8 clocks | `pixel_tb.v` /RE SPACING law |
| skip caps | 14, 12, 14, 12 | `pixel_tb.v` `skip_fine_of()` (both clamps), asserted over the 65-combination matrix |
| record + 1 | 1 extra word, never 2 | `pixel_tb.v` `skip_reads_len()` allowance |
| palette write window | IDX stolen clocks 1..3, pulse at 2 | `clut_pair_tb.v` S7, `pixel.v` `wr1`/`wr2` |
| pixels painted wrong | commit+2..+4 in clock terms = the three pixels after the SET's own slot | `clut_pair_tb.v` S6-neg + LANDING-NOTES §4 |
| entry cost | 3 VIDCMD words | `clut_pair_tb.v` S1a/S1b (both illegal third words DETECTED) |
| PIXEL_OUT_LEAD | 1 (unchanged) | `pixel_tb.v` case R2 output latency |

## Layer 2 — SHARED MODEL (`super-engine/{descriptor.h,render.h,render.cpp}`)

`descriptor.h`

* The PIXELS-stream section is rewritten: the depth enum + half-rate flag
  (`pixel_mode()`, `pixel_mode_depth()`, `pixel_mode_half_rate()`),
  `pixels_bits_per_group()`, `pixels_fetch_trigger()`, the skip law in three
  pieces (`pixels_skip_depth_clamp()`, `pixels_skip_cap()`,
  `pixels_skip_effective()`), `pixels_record_words()`, `pixels_mode_defined()`
  (4bpp full rate is REFUSED, not computed) and `pixels_words_per_line()` =
  record + (skip != 0).  Every one of the seven word counts and the four skip
  caps is `static_assert`ed against the `griffin.yml` constants.
* `PIXEL_FETCH_LEAD` (16) and `PIXEL_LAST_CLOCKS` (8) are named here; the SET
  targets are renumbered and `static_assert`ed against the generated header;
  `SET_FILLER` is target 7.
* NEW: `PALETTE_ENTRIES`, `PALETTE_WORDS_PER_ENTRY` (3),
  `PALETTE_WRITE_MEASURED_SLOTS` (3), `PALETTE_WRITE_DISTURBED_SLOTS` (4),
  `palette_index_1bpp/2bpp()`, and `vidcmd_palette_entry(index, colour)` —
  the authoring helper that emits the three words so nobody hand-rolls them.
* Retired: `PIXEL_MODE_MICRO_HAM`, `PIXEL_MODE_INDEXED_2BPP`, `IDX2_CODE_*`,
  `HAM_CODE_*`, `pixel_mode_ham()`, `pixel_mode_indexed()`,
  `PIXELS_WORDS_MICROHAM`, `PIXELS_WORDS_INDEXED2`, `SET_PIX_PAL_FG/BG`,
  `SET_PIX_HAM_HELD`.

`render.h` / `render.cpp`

* **`PixelUnit` is now a clocked state machine, register for register with
  `pixel.v`**: `shift_reg`, `bit_pos`, `align_cnt`, `half_phase`,
  `fifo_loading`, `line_skipped`, `extra_fetch_done`, `attr_colour`,
  `col_transfer`, `grp`, `pix_consume_d`, `IDX`, and the same priority chains
  (the engine's `fifo_loading` branch preempts its `step` branch; the shifter
  sits outside that chain).  The old model had NO fetch cadence — it refilled a
  bit buffer on demand — so it could not observe a line's read count at all.
  `line_words_read()` is that count now.
* The three raster strobes are the RTL's inputs: `arm_preload()` +
  `blank_clock()` give PIX_PRELOAD at `H_BLANK - PIXEL_FETCH_LEAD` (the drivers
  arm it, and only before a VISIBLE line), `next_pixel()` is a PIX_CONSUME clock
  and derives PIX_LAST from its own line-relative counter.
* **Two bugs the RTL-faithful rewrite found in my own first cut**, both worth
  recording because both would have produced a confident wrong answer:
  1. resolving the pixel from the SHIFTER instead of from `grp` broke half rate —
     the shifter has already moved on, and it is the `grp` REGISTER that holds a
     group across the two clocks of a pair.  Caught by the 4bpp/attr half-rate
     traces on the first run.
  2. the run's LAST line armed a preload for a line that never renders, which
     read a PIXELS word nobody deposits and showed up as one tiled word per
     frame in every case.  Both drivers now suppress it.
* NEW `PaletteUnit`: sixteen 12-bit entries, `reset_power_on(seed)` fills them
  with a reproducible xorshift pattern (NEVER zeros, never cleared by `/RS` —
  `frame_reset()` only clears the write sequencer), the measured write sequence
  as a four-phase counter, `bus_disturbed()` / `bus_undefined()` /
  `write_phase()`, and two violation counters.  It lives inside `PixelUnit`
  because PIXEL drives the address and the pulse; the DATA comes from
  `CompositorUnit`'s new `set_pix_value_` register, which is re-registered from
  the staged word on every clock exactly as `compositor.v` does.
* `CompositorUnit::power_on_reset()` is new and is **not** the `/RS` state — see
  the disagreement below.  It also flags a disturbed pixel that reaches the DAC
  through passthrough (`palette_disturbed_visible`).
* `RenderStats` gains `palette_writes`, `palette_bad_filler`,
  `palette_disturbed_visible`, `palette_unwritten_reads`.

## Layer 3 — SUITE (`super-engine/{main.cpp,author.h,author.cpp}`)

`author` layer: `PixelMode` is the depth enum plus half rate
(`DIRECT_1BPP`, `TWO_BPP`, `ATTR`, `HALF_1BPP`, `HALF_TWO_BPP`,
`HALF_FOUR_BPP`, `HALF_ATTR`); `RecordWriter::palette_entry()` and a per-line
`put_entry()` emit three-word entries; `FrameParams` swaps
`frame_ham_held`/`ham_held_color` for `palette[16]` + `palette_entries` +
`pix_base`; the frame's palette is deposited **in vertical blanking** by
`author_frame()` (48 words is more than an HBLANK delivers — proved by getting
this wrong first: a 48-word line-0 HBLANK packet slipped the whole list and
every line overran); `write_test_pattern_microham()` is gone and
`write_test_pattern_indexed4_half()` / `write_test_pattern_attr()` are new.
`mid_line_split` and `slot_regression` now switch PALETTE PAIRS with
`SET(pix_base)` instead of writing colour registers — a mid-line palette WRITE
would be illegal under passthrough, and a base switch is one word and
pixel-exact, so the cases keep their assertions and gain a new law.

| case | status |
|---|---|
| `1BPP_REGRESSION`, `HALF_1BPP` | ported into the new mode matrix |
| `HAM_REGRESSION`, `HAM_HALF_MASKED`, `MODE_RESERVED_11`, `IDX2_ALL_FOUR`, `HALF_IDX2`, `IDX2_SET_ORDER`, `IDX2_SKIP_CLAMP`, `IDX2_UNDERRUN` | **RETIRED** with micro-HAM and the fixed four colours |
| `m2-microham` | **RETIRED**, replaced by `m2-attr` |
| `m8-indexed2` | **RETIRED**, replaced by `m8-idx2bpp` |
| `m9-halfrate-idx2` | **RETIRED**, replaced by `m9-halfrate-attr` |
| new `pixel-modes` matrix | 7 depth/rate traces + `BASE_PAIR` + `BASE_QUAD` + `SKIP_MATRIX` (108 cells) + `SKIP_TAIL` + `RESET_1BPP` + `PALETTE_UNLOADED` |
| new `palette` traces | `S1c`, `S1a`, `S1b`, `S3`, `S6`, `S6-neg`, `S8` |
| new screen cases | `m2-attr`, `m10-idx4-half`, `m9-halfrate-attr` |
| ported screen cases | `m0-slot-regression`, `m1-vscroll-jit` (3 -> 9 preamble words), `m2-perline`, `m2-split`, `m8-idx2bpp`, `m9-halfrate`, `m3-cursor`, `m3-sprites`, `m4-audio`, `m5-handshake`, `m6-console`, `m7-kiosk`, `crazy-demo`, `tempest-web`, the cadence and MASK traces |

`check_case()` now asserts `palette_unwritten_reads == 0`,
`palette_bad_filler == 0` and `palette_disturbed_visible == 0` for every case,
and `print_report()` prints all four palette counters.  **That guard fired
immediately on `m3-cursor` and `m3-sprites`**, which loaded no entries at all
and were painting the RAM's random power-up contents: the negative control the
brief asked for, produced by accident on the first run and then fixed by giving
both cases their two playfield entries.

## Layer 4 — EMULATOR and FIRMWARE

`emulator/emulator.cpp` inherits the model through `render.cpp` and needed three
changes: a **power-on reset** of both units at startup (the palette RAM's random
contents and COMPOSITOR's POWER_RESET state, neither of which is the `/RS`
state), `run_blanking(..., line < V_ACTIVE)` so PIX_PRELOAD is armed only before
a visible line, and nothing else — it never named `pal_fg`, `ham_held` or
micro-HAM.  It rebuilds clean.

`firmware/rom.cpp`

* `dl_build_console_list()`: the frame preamble goes from **3 words to 9** —
  palette entry 0 (background), palette entry 1 (foreground), `SET pix_base 0`,
  `SET pix_mode 1bpp`, `RUN(passthrough,1)` — deposited with no `wait_hblank`,
  i.e. in vertical blanking, which is the only place a palette write is
  unconditionally legal.  `CONSOLE_PREAMBLE_WORDS` replaces the hard 3 in the
  budget `static_assert`.
* The per-line-palette image viewer's packet goes from 3 words to **7** (two
  three-word entries + the RUN) and the viewer gains a two-word frame preamble
  for `pix_base`/`pix_mode`, which `/RS` clears every frame.
* **The micro-HAM viewer is deleted** (`dl_build_ham_viewer_list`,
  `dl_emit_ham_pixel_line`, the HAM geometry constants and the HAM branch of
  `view_image`).  `view` now refuses a 76800-byte plane by name: "micro-HAM is
  retired (PIXEL is index-only now) and there is no viewer for it".  The help
  text and the usage message follow.  A 4bpp or attribute-mode viewer is the
  replacement and is package 3's.

### Gates

| gate | result | evidence |
|---|---|---|
| repo-root `make` + `make check-generated` | PASS | — |
| `super-engine` `make check` | PASS, all cases | `package2-evidence/super-engine-suite.txt` |
| emulator rebuild | clean | — |
| firmware build (project toolchain wrappers) | clean, `rom.bin` 127,328 bytes | — |
| firmware boots headless to a correct console | PASS | `package2-evidence/console-shot-after.png`, `console-after.txt` |
| screenshot vs the PRE-CHANGE emulator+ROM | same picture | `package2-evidence/console-shot-before.png` |
| `apps/dmabench/check-dma-stall.sh 40` | PASS C1 76.1 %, C2 55.9 % | `package2-evidence/dmabench-check.txt` |
| negative control: console palette load broken | picture goes wrong | `package2-evidence/console-shot-negative-control.png` |

**The before/after screenshots.**  Taken the same way (`--headless
--no-throttle --console-in` a single `help`, `--screenshot`, 120 M cycles), the
"before" one from the tree as it stood at `f39178b` with its own `rom.bin`.  The
two pictures are the same console, white on black, same layout, same splash;
they differ only where the TEXT differs — the build stamp, `3 VIDCMD words` ->
`9 VIDCMD words` on the ENGINE line, and the `view` help line.  I compared them
by eye, side by side, rather than by pixel diff, because those text differences
make a byte compare meaningless.

**The negative control** replaced the preamble's two palette entries with no-op
filler SETs and changed nothing else.  The console then painted **teal text on a
purple background** — the palette RAM's reproducible random power-up contents —
with the layout intact, which is exactly the failure mode the model promises: an
unloaded entry is not black, it is noise.  Restored immediately after; the
firmware in the tree is the good one.

**dmabench's analytic number is unchanged**, and the reason is worth stating:
C1 and C2 are `apps/dmabench`'s OWN display lists out of a prebuilt `.bin` and a
prebuilt CF image, not the firmware console's, and neither was rebuilt (apps are
package 3).  What did change is the firmware console's per-FRAME VIDCMD cost,
3 words -> 9, which is +12 SYSCLK in a 233,000-SYSCLK frame — 0.005 %, well
inside the +/- 2 pp the script allows.  Words per LINE are unchanged: 40 PIXELS
and 0 VIDCMD on 479 of the 480 lines.

## Disagreements found

1. **COMPOSITOR's power-on state: the brief vs `LANDING-NOTES.md`.**  The notes
   say the power-on reset has "the same effect as `/RS`: held colours to
   0xFFF/0x000".  The brief says the state before the first `/RS` is "the
   power-on clear (all zero)".  I implemented the BRIEF — `power_on_reset()`
   leaves both held colours 0x000 — because POWER_RESET clears macrocell
   registers rather than applying the RTL's `/RS` values, and because the
   conservative version is the one that makes a list depending on the `/RS`
   white fail before the first vsync instead of passing by accident.  The suite's
   cadence and MASK traces are lines INSIDE a frame, so they call `reset()`
   (i.e. post-`/RS`) and are unaffected.  If the owner prefers the notes'
   version it is a one-line change in `CompositorUnit::power_on_reset()`.
2. **`LANDING-NOTES.md` §4 "the filler owns clocks 2 and 3".**  Derived
   mechanically from `compositor.v` (the bus is registered from the staged word
   every clock) plus the one-clock commit lead, the word behind the data SET
   owns clocks **1 and 2**, not 2 and 3 — the pulse at clock 2 sees it either
   way, which is why no measured outcome changes and why `clut_pair_tb.v`'s hold
   measurement (4 clocks with the contract) cannot tell the two apart: its case
   (c) has nothing behind the filler.  The model therefore asserts the criterion
   the bench actually settled on — "the data AT THE PULSE is the payload the
   `clut_data` SET committed" — and does not check clock 3, where a normal list
   has already staged the next word.  Everything the bench measured (contract
   stores V; either illegal third word stores ITS payload) is reproduced.
3. **`pixel.v`'s `//PIN` block vs its own reserved-pin comment.**  The header
   says "RESERVED, not fitted: DUART OP4 -> 54, OP5 -> 55, OP6 -> 56, OP7 -> 57"
   but the `//PIN` block assigns `PIXELS_Q_0 : 56` and `H_ACTIVE : 57`.  Two of
   the four reserved pins are occupied by the fit that shipped.  Not touched
   here (package 1's file, and pinouts are not this package's business), but it
   must be resolved before the rev-2 pinout is frozen.
2. **LANDING-NOTES §2's pin list vs the `//PIN` block.**  §2 lists the numbers
   from the earlier harvest (e.g. `nPIXELS_RE` 15 agrees, but `IDX` 24/27/28/29
   and `nCLUT_WE` 30 against the block's 16/24/27/33 and 58).  The `//PIN` block
   is the one home, per CLAUDE.md, so it wins; the notes' §2 is stale prose.

## Package 3 must do

* `apps/` — `dmabench.cpp` and `smurf.cpp` still use `SET_PIX_PAL_FG` /
  `SET_PIX_PAL_BG` / `SET_PIX_HAM_HELD` / `PIXEL_MODE_MICRO_HAM` and will not
  compile against the new `descriptor.h`.  Their prebuilt `.bin`/CF images are
  what this package's gates use, so nothing was rebuilt there.
* `image-tools/` — `microham.h`, `microham_roundtrip.cpp` and the `.ham`
  fixtures; `clut-experiment` promotion to a real encoder.
* the griffin-ports Smurf app; `board/` (the palette RAM, the eight extra Q
  traces, IDX/nCLUT_WE, the retired TIMING socket, the JTAG chain);
  `griffin-video-datasheet.html` status stamps; `reminders.md`.
* `firmware` image viewer: the micro-HAM path is DELETED (see Layer 4), so
  `.ham` files are refused with a message rather than rendered.  A 4bpp or
  attribute-mode viewer is the natural replacement, and it would want the
  `clut-experiment` encoder.
* **A JIT deadline the model can now see and nobody has checked against the
  board**: PIXEL reads a line's FIRST word at PIX_PRELOAD, 16 pixel clocks
  before the line, not at pixel 0.  The model reads on the chip's clocks now, so
  a list whose first word lands inside those 16 clocks tiles one word.  No suite
  case does today, but the margin is real and it is the kind of thing the old
  model could not express.
* Nothing in `board/` was touched: the palette RAM, the eight extra PIXELS Q
  traces, IDX/nCLUT_WE, the retired TIMING socket and the five-TAP JTAG chain
  are all still rev-1 there.
