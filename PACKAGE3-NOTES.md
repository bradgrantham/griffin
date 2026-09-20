# Package 3 — apps, image tools, the firmware viewer, the docs

Package 1 (88a1e3c, f39178b) landed the RTL and the chip testbenches; package 2
(a6e0187) propagated the indexed PIXEL through the definitions, the shared
model, the suite, the emulator and the firmware console.  This package finishes
the job at the edges: everything that authors a display list or makes a picture.

Nothing is committed by this work.  `griffin.log`, `griffin-video-datasheet.html`
and `vidcmd-literal-hold-proposal.md` are deliberately untouched.

**One accident to know about, and it is mine.**  Retiring the micro-HAM tools
was done with `git rm`, which STAGES the deletion.  While this work was running
the owner made three commits on the branch (`9f7e74a`, `0a846f5`, `9ae0400`, all
on `vidcmd-literal-hold-proposal.md`), and the last of them swept those staged
deletions in with it: `9ae0400 "proposal: alternative B, direct-load zero-slot
literal for 640-res patches"` also deletes `microham.h`,
`microham_encode.cpp`, `microham_roundtrip.cpp` and the six
`smurf-assets/*.{ham,pal}` files.  The deletions are correct and are what this
package intends; they are simply in a commit whose message says something else,
and it is the owner's history to tidy (or not).  Everything else this package
touched is unstaged in the working tree.

**Order note.**  The brief's items are apps, then image-tools, then firmware.
`apps/smurf` and the firmware viewer both need the new encoder, so item 2 was
done between `apps/dmabench` and `apps/smurf`.  Everything that built before
each step still built after it.

---

## 1. `apps/`

| app | what changed |
|---|---|
| `dtest` | **nothing.**  It is all-VIDCMD — no PIXELS descriptors, no passthrough, no palette entry is ever read — so it compiled and ran unchanged against the new headers.  Gate below all the same. |
| `hello`, `exctest`, `basic` | nothing; they never touched video.  All three rebuild clean. |
| `dmabench` | ported; see below. |
| `smurf` | ported to a 4bpp half-rate backdrop; see below. |
| `apps/lib` | **nothing.**  `griffin_video.{h,cpp}` is the ownership syscalls and the vblank poll; it names no video register. |

### `apps/dmabench`

Every configuration's **words per line is unchanged**, deliberately, because
that is what `RESULTS.md`'s analytic column is computed from.  What changed is
what the words are.

* New `put_preamble()` emits palette entries + `SET pix_base` + `SET pix_mode`
  as ONE VIDCMD descriptor with no `wait_hblank`, i.e. in vertical blanking —
  the only place a palette write is unconditionally legal and the only place a
  sixteen-entry load would fit.  C1 and C4 use it; the cost is one descriptor
  per FRAME.
* **C1** (1bpp console): the 3-word preamble becomes 9 (two three-word entries,
  base, mode, the 1-slot RUN) — byte for byte what `rom.cpp` now emits.
* **C2** (pure-VIDCMD MASK text): line 0's `SET pix_pal_bg` / `SET pix_mode`
  became `SET pix_base` / `SET pix_mode`.  Still two words, so 82/84 stands.
  Its comment now also says why dibit 00 is still not used for the picture: it
  would now be an UNWRITTEN palette entry, which the RAM powers up random.
* **C3** (was "2bpp indexed full rate"): still 80 PIXELS words and **still six
  VIDCMD words per line**, now a three-word palette entry for entry 3 plus the
  two COMPOSITOR colour SETs plus the 640-slot RUN.  So C3 still reloads one
  colour every scanline, which is the thing it exists to price.  The other three
  entries a 2bpp line reaches are loaded once per frame.
* **C4**: the playfield's two entries and its mode moved to the preamble, and
  **a latent bug is fixed**: C4 authored 61 words a line and streamed 60, so the
  trailing `RUN(passthrough,192)` was dropped on every line of every frame.
  Authored and streamed are both 60 now (`SET cmp_color1`, `SET cmp_color0`,
  `SET pix_base`, 28 MASK records, the RUN), and the app now complains if they
  ever disagree again.

`RESULTS.md` gained a dated section stating all of the above plus the
re-measurement; the 300-field table is left as the record it was.

### `apps/smurf` — the backdrop mode, and why

**4bpp half rate, 320x480, with a PER-SCREEN sixteen-entry palette** (the
`.gc4` files `image-tools/clut_encode --k 0` writes).  Not attribute mode.

* It is the **same 80 words per line** micro-HAM used, so `FRAME_DESCRIPTORS`,
  `PIXEL_DESCS_PER_LINE`, the 15,728-byte table, the 32 KB carve arithmetic and
  the audio pacing are all untouched.  Only two descriptors were added, for the
  frame preamble.
* Attribute mode is the other 80-word option and keeps 640 horizontal
  resolution, but allows only two colours per eight-pixel cell.  These backdrops
  are painted scenery with **soft vertical gradients**, which is exactly what an
  attribute cell cannot hold; the sprites are a COMPOSITOR overlay in the
  640-pixel domain and never wanted the extra horizontal resolution.  Halving
  the backdrop's horizontal resolution changed none of the span arithmetic.
* The palette is loaded **once per frame in vertical blanking** (sixteen
  three-word entries plus base and mode, fifty words, two descriptors), not per
  line.  `/RS` clears `pix_mode` and `pix_base` at every vsync so the preamble
  has to run anyway; the entries ride along for nothing, and the per-line packet
  stays entirely available for sprite spans.
* `emit_packet()` **lost its three leading SETs** (`pix_mode`, `pal_fg`,
  `pal_bg`), so every packet is three words shorter: the widest sprite pair is
  now 20 words against the 24-word cap and the widest text line 19.  A mid-line
  palette WRITE would be illegal anyway — the four pixel clocks after one are
  disturbed and nothing covers them on an active line.
* Assets: three `.ham` + three `.pal` files become three `.gc4` files, read with
  `image-tools/gc4_target.h` and streamed **in place** (the format's pixel
  section is contiguous and fixed stride so no copy is needed).

---

## 2. `image-tools/`

### Retired

`microham.h`, `microham_encode.cpp` and `microham_roundtrip.cpp` are **deleted**
(`git rm`), with their Makefile targets and the `check` rule that drove them;
so are the six `smurf-assets/*.{ham,pal}` files.  `microham.txt` at the repo
root is left alone — it is the owner's format note, not a tool.

The blue-noise ordered-dither mask was the one thing in `microham.h` worth
keeping (it is a general-purpose dither, not a micro-HAM mechanism), so it moved
to the new `clut_color.h` rather than to `obsolete/`.

### Promoted

Four new files, and one extraction that gives the heuristic **one home**:

| file | what |
|---|---|
| `clut_color.h` | R4G4B4 arithmetic + the blue-noise mask, out of `clut-experiment/clut_format.h` and `microham.h` |
| `clut_quant.h` | `line_palette()`, `update_clut()`, `nearest_entry()` and the FS constants, out of `clut-experiment/clut_encode.cpp`, **unchanged arithmetic** |
| `gc4_format.h` / `gc4_target.h` | the `.gc4` file; the second is freestanding (`<stdint.h>` only) and is what the firmware and `apps/smurf` include.  `gc4_format.h` `static_assert`s the two layouts agree |
| `clut_encode.cpp` | the tool: source image -> `.gc4` + preview PNG |
| `clut_decode.cpp` | independent decoder **and** validator |
| `clut_roundtrip.cpp` | the suite-model round trip, mirroring `microham_roundtrip`'s structure |

`clut-experiment/` keeps its modes, its K/N sweep, its span planner and its
recorded `out/` — it is the record — but now **includes** the shared headers
instead of carrying its own copy.  Proof the extraction changed nothing: four
configurations re-encoded and byte-compared against the recorded sweep
(`c4h K4 N0 fs`, `c4h K16 N8 ordered`, `attr K12 N4 none`, `c2f K4 N0 fs` on
maya2) — **all four identical**.  Its `mham` baseline rung is now skipped when
`../microham_encode` is absent, which it always is; its recorded rows stay in
`out/results.csv`.

### The `.gc4` format, in three lines

* 48-byte big-endian header: `'GCL4'`, version, K, width 320, height, 80
  words/line, the `SET(pix_mode)` value, then the sixteen-entry initial palette.
* Then `height x (1 + K)` words of palette records: a load count, then K slots
  of `{index[15:12], colour[11:0]}` — one word per load, which is exactly the
  payload the two SETs of a palette entry carry.
* Then `height x 80` words of 4bpp half-rate pixels, contiguous and fixed
  stride, so ENGINE streams them in place and line n is a multiply.

K is 0..8, default 4.  The ceiling is 8 because `8 * 3 + 1 = 25` words is what a
26-word HBLANK packet holds; K = 0 means "the header's palette is the whole
palette", which is what a game backdrop wants.  **No refinement spans** — a span
is a COMPOSITOR record that has to be placed against everything else the line's
packet is doing, and that is the display-list planner's job, not an image tool's.

### Two encoder decisions worth recording

1. **The destination box is the 640-wide OUTPUT, not the mode's 320 groups.**
   Half rate halves the horizontal SAMPLING, not the picture's shape.  Fitting
   the source to a 320x480 box centre-crops it to half its width and then
   stretches it back; the first meadow backdrop out of this tool was exactly
   that and it was obvious against the micro-HAM preview.  Fit to 640 x height,
   then box-average pairs into the 320 groups.
2. **The K = 0 global palette is weighted by `sqrt(count)`, not by area.**
   Straight area weighting is what a k-means wants and it throws away every
   small bright thing: the first K = 0 meadow lost the sun, both clouds and the
   brown fence, spending all sixteen entries on the sky and grass gradients.
   `sqrt` pulls "most of the picture" against "a small distinct object" from
   about 1000:1 to 30:1, which is enough, and the gradients lose one step each.

---

## 3. `firmware/`

`view` keeps its name and gains a one-argument form:

    view IMAGE            a .gc4 picture, 320x480 4bpp half rate
    view IMAGE PALETTE    the older 1bpp plane + per-line R3G3B2 palette

* `dl_build_gc4_list()`: a 50-word vblank preamble (sixteen entries, base,
  mode), the 34-line walk, then per line one VIDCMD descriptor of that line's
  K loads plus `RUN(passthrough,1)`, then 80 PIXELS words as 32/32/16 —
  **streamed straight out of the loaded file**, so nothing is copied into the
  framebuffer and the console's plane survives the viewing.
* The per-line packets are an ordinary `malloc`, not the display-list payload
  region: 480 x 25 words is 24 KB and the region is 8 KB, and only the
  descriptor TABLE has to live inside edma3's hard-wired page.
* `static_assert`s: the list fits the table region; a K = 8 packet still leaves
  the line's first PIXELS word inside HBLANK (`20 + 2W + 15 <= HBLANK_SYSCLK - 2`
  with W = 25); `Gc4Target::WORDS_PER_LINE == PIXELS_WORDS_4BPP_HALF`.
* `view_wait_and_restore()` factored out of `view_image()` and shared.
* `firmware/Makefile` now **checks the ROM size** after `objcopy` and fails the
  build above 131,072 bytes (the W27C512 pair).  `linker.ld`'s ROM region is the
  1 MB the decode covers, not the 128 KB that is stuffed, so nothing was
  checking this.

### ROM size

| | bytes | free of 131,072 |
|---|---:|---:|
| before (HEAD) | 127,320 | 3,752 |
| after | 130,508 | **564** |

That is tight and it is the headline risk in this package.  The viewer is ~3.2 KB
of code; nothing was removed to pay for it.  The 1bpp `view IMAGE PALETTE` path
is the obvious thing to delete if the next feature needs the room.

### A firmware finding I could not explain

`dl_build_gc4_list()`'s preamble is a **`static`** array, and the comment says
why.  As a 50-word LOCAL array, its copy into the display-list payload region
came out **shifted by one word**: the region's first word read back as zero
(`m0 0000` against `a0 A000`) and the last word — `SET(pix_mode)` — was never
deposited, so PIXEL stayed in its `/RS` 1bpp mode and a 4bpp picture rendered as
noise.  The evidence, in order:

* The emulator's PIXEL model reported `mode 0` on every line of every frame.
* A trace of deposited VIDCMD words showed 49 of the 50 arriving, always missing
  the last one — swapping `SET pix_base` and `SET pix_mode` moved which one was
  lost, proving it was positional and not about the value.
* A firmware read-back of the payload region immediately after the copy showed
  `mem[k] == preamble[k-1]`, with `mem[0] == 0`.
* The monitor's `write`/`read` at `0x3FE000` works perfectly, so it is not RAM.
* `dl_put_vidcmd()` is **not** the culprit: the same call with a `static` source
  array is correct.  Local array -> wrong; static array -> right; reproducible.

Toolchain is the project's m68k-unknown-elf 15.2.0 at `-O3 -fanalyzer`.  I did
not chase it further — `.bss` is the right home for a 100-byte constant table in
any case — but it is worth a look, because **the 1bpp viewer builds its 480
per-line packets from a 7-word local array the same way** and was not exercised
by any gate here.

---

## 4. Docs

### `griffin.md`

Rewritten to current fact, not annotated: the video path is "three chips and a
RAM" (`ENGINE -> FIFOs -> PIXEL -> 16x12 palette RAM -> COMPOSITOR -> DAC`); the
TIMING chip entry is gone and its contents are PIXEL's "Raster" bullet with
`timing.v` named as a module instantiated inside `pixel.v`; PIXEL's unpacker
bullet states index-only, the ganged 16-bit PIXELS port, the four depths with
their word counts, half rate and the `pixel_skip` rule; a new **Palette RAM**
bullet states the three-word entry, the four disturbed clocks and that nothing
ever resets it.  Every other `TIMING` reference — the pixel clock, GLUE's vsync
latch, PORTS' two ticks, the level-6 IRQ, the JTAG chain (now five CPLDs), the
frozen-pinout list, the Rev-2 chip list and four board-checklist items — now
says PIXEL, and the checklist gained the palette RAM.  The open question "Video
through a CLUT RAM?" is struck through and answered.

### `reminders.md`

**Not edited.**  It holds audio and RS-232 refreshers; nothing in it mentions
video, TIMING, micro-HAM or the palette, so nothing there is now wrong.

### `griffin-video-datasheet.html` — NOT edited (owner's).  What is now stale

Read in the working tree.  It is already the rev-3 proposal revision, so it is
right about micro-HAM, the palette RAM and the merged chip; what has moved under
it is the STATUS of those sections and a handful of statements.

**Status stamps that can be promoted.**  The header says "Sections stamped
PRELIMINARY are fitted and simulated as experiments but not yet production RTL,
model, suite or emulator".  That sentence is now false for three sections:
packages 1 and 2 landed production `pixel.v` with `pixel_tb.v`/`timing_tb.v`/
`clut_pair_tb.v`, the shared model, the suite and the emulator, and this package
put them under three applications and the firmware.

* **§3.1 PIXELS stream — indexed data plane** PRELIMINARY -> production in RTL,
  model, suite, emulator, `apps/smurf`, `apps/dmabench` C3/C4 and the firmware
  viewer.  Still OPEN against hardware (no board).
* **§3.6 Palette RAM — load and reload** same.  The three-word entry is now an
  authoring helper (`vidcmd_palette_entry()`), and its "16 entries in vblank =
  48 words" is exactly what `apps/smurf` and the firmware viewer emit (50 with
  the base and mode SETs, two descriptors).
* **§3.7 Half-rate flag** same; `apps/smurf` is a half-rate 4bpp screen and
  `apps/dmabench` C4 a half-rate 1bpp one.
* **§7's line "Production pixel.v, pixel_tb, timing_tb on merged top, chip-pair
  palette-write bench … PASS 2026-09-20 (branch)"** can add: model, suite,
  emulator, apps and firmware all on the same branch, and the encoder's output
  renders pixel-exact through the model.

**Statements that are now wrong**

* **§3.1: "On underrun PIXEL re-shifts zeros: index 0 of the current base."**
  This contradicts `vidcmd-spec.md` §2, `descriptor.h` and the model, which all
  say a 7200 **holds Q** while empty, so an underrun **tiles the last word** —
  `RenderStats::pixels_tiled_words` counts exactly that, and `clut_roundtrip`
  asserts it is zero.  "Re-shifts zeros" was rev-1's behaviour and was what made
  a short fill paint BACKGROUND to end of line; it is not what the indexed part
  does, and a short-fill-as-compression recipe built on it would paint garbage.
  This one is worth settling against `pixel.v` before anyone relies on either.
* **§3.10's "Pin numbers in this section are the retired ATF1504 part's"** and
  **§7's "plus pins 20/21/24/25 reserved for DUART OP4..OP7"**: the merged chip
  reserves 54–57 per `pixel.v`'s header, and two of those four are occupied —
  see the next section.  §3.10's "The DUART OP4..OP7 reservation follows the
  logic" is the sentence that has to be made true.
* **§7's "Passthrough-as-third-color (empty PIXELS → dibit 00 = pal_bg)"** still
  names a retired register; §5 has it right (palette entry `{pix_base, 0}`).
  Both should also say that the entry is one **software must load** — the RAM
  powers up undefined, so an unloaded third colour is noise, not black.
  `apps/dmabench` C2's comment now says so and deliberately does not use it.
* **The masthead's "sources: indexed-pixel-spec-draft.md"**: that draft was
  folded into `vidcmd-spec.md` by package 2 and carries a FOLDED IN header.
* **§5 "Image viewer … PRELIMINARY (host experiment)"** and its note pointing at
  `image-tools/clut-experiment`: the experiment has been promoted.  The shipping
  path is `image-tools/clut_encode` -> a `.gc4` file -> `view IMAGE` in the
  firmware, and the round trip through the model is `clut_roundtrip`.  The
  recipe's "optionally refined by up to 16 exact-colour RUN spans per line" is
  **not in the shipped tool** — spans need the display-list planner, and the
  span experiment stays in `clut-experiment/` as the record of what they were
  worth.  The 3–5x-better-than-micro-HAM error claim is unaffected; it was
  measured on the same encoder core, which is now shared verbatim.

**Now confirmable**

* **§5 "Image viewer — stills ≈85–130 words/line"**: measured.  K = 4 on a real
  photograph (maya2) costs 3.24 palette loads per line, i.e. 80 pixel words plus
  13 VIDCMD words = **93 words/line**; K = 8 is 105.  Both inside the estimate.
* **§5 "16-color bitmap — 320×480 … CPU keeps ~52%"**: `apps/dmabench` C3 is
  that line's shape (80 PIXELS + 6 VIDCMD words, four descriptors) and measures
  **51.4 %** of the no-DMA throughput against an analytic 51.59 %.
* **§5 "Text console — ≈43 words/line · CPU keeps 76%"**: `apps/dmabench` C1 is
  the firmware console byte for byte, including the nine-word vblank preamble,
  and measures **76.1 %** against an analytic 76.24 %.
* **§5 "Tile game … 16-colour alternative: 4bpp half rate at 80 words/line"**:
  that is exactly what `apps/smurf` now is, and it runs at **59.79 fps with 0
  late frames** and 0 packet truncations under the emulator's DMA cost model.
* **§3.6's disturbed window and the filler rule**: a whole application and a
  whole firmware command now obey them, and the model's three palette guards
  (`palette_unwritten_reads`, `palette_bad_filler`,
  `palette_disturbed_visible`) are asserted zero over nine full-frame round
  trips in `make -C image-tools check`.
* **The 26-word HBLANK packet budget** (`20 + 2W + 15 <= HBLANK_SYSCLK - 2`) is
  what sets the `.gc4` format's K ceiling at 8: `8 * 3 + 1 = 25` fits, 9 does
  not.  Worth stating in §3.6 next to "16 entries in vblank = 48 words", because
  it is the per-LINE counterpart of that number.

### `cpld/pixel/pixel.v`'s `//PIN` block vs its own header — NOT edited

The header at line 755 says

> RESERVED, not fitted: DUART OP4 -> 54, OP5 -> 55, OP6 -> 56, OP7 -> 57 …
> The schematic must route them and no future output may be placed there.

while the `//PIN:` block already places `PIXELS_Q_0 : 56` and `H_ACTIVE : 57`.
Two of the four reserved pins are occupied by the fit that shipped.

**Suggested resolution: move the RESERVATION, not the fitted pins.**  The
reservation has no logic, no net and no board behind it yet, so moving it costs
a comment; moving `PIXELS_Q_0` or `H_ACTIVE` costs a re-fit with
`-preassign keep` plus the production strategy flags, and (once routed) a board
respin.  Pins **54 and 55 are unassigned** in the `//PIN` block, so two of the
four already land; pick two more from PIXEL's unassigned I/O, keep all four
adjacent if the connector prefers it, and correct the header comment in the same
edit.  Whichever way it goes, it must be settled **before the Rev-2 pinout is
frozen for routing** — `griffin.md`'s board checklist now says so.

---

## 5. The sweep

`grep -ri "micro-ham\|microham\|pal_fg\|pal_bg\|ham_held\|HAM_"` over
`*.{cpp,h,py,md,yml,mk}` and `Makefile`, excluding `cpld/experiments/`,
`obsolete/`, `old/`, the vendored `image-tools/stb/`, the imported
`from-alice3/` and `from-old-rocinante/` trees, `image-experiments/`,
`emulator/build/` and the proposal/draft/notes files.

**Fixed by this package**: `apps/smurf/smurf.cpp`, `apps/dmabench/dmabench.cpp`,
`apps/dmabench/RESULTS.md`, `firmware/rom.cpp`, `image-tools/Makefile`,
`image-tools/make_smurf_assets.cpp`, `image-tools/smurf-assets/README.md`,
`griffin.md`, and the deleted `microham*.{h,cpp}`.

**Remaining hits, each with its reason:**

| file | hits | reason |
|---|---:|---|
| `image-tools/{clut_color.h, gc4_format.h, clut_roundtrip.cpp}` | 3 | one sentence each saying what was retired and where its dither mask went.  Deliberate. |
| `image-tools/smurf-assets/README.md` | 6 | the dated "was micro-HAM, now 4bpp half rate" paragraph and two back-references. Deliberate. |
| `apps/smurf/smurf.cpp` (2), `apps/dmabench/dmabench.cpp` (4), `apps/dmabench/RESULTS.md` (3), `firmware/rom.cpp` (9), `image-tools/make_smurf_assets.cpp` (1) | 19 | ditto: each is a comment stating what the code used to do and why the replacement is shaped the way it is.  `rom.cpp` also still refuses a 76,800-byte `.ham` plane BY NAME, which is a feature. |
| `image-tools/clut-experiment/{DESIGN.md, Makefile, clut_compare.cpp, clut_format.h, clut_prepare.cpp, report.py}` | 20 | **the record.**  micro-HAM was this experiment's baseline rung and its numbers are in `out/results.csv`; deleting the mentions would make the recorded table unreadable.  The rung is skipped at run time now. |
| `super-engine/{main.cpp, author.h, author.cpp, descriptor.h, render.cpp}` | 17 | **stale prose in package 2's layer**, not code: case titles and comments that still say `pix_pal_fg` / `pix_pal_bg` where the case has been ported to palette entries, plus two `descriptor.h` lines that correctly record what died.  The suite passes; these are a cleanup for the owner, listed rather than edited because the brief put that layer out of scope. |
| `cpld/pixel/LANDING-NOTES.md`, `cpld/Makefile` | 12 | package 1's landing notes and two Makefile comments recording fit history (`pixel_combined.v`, the VIDEO xor_synthesis result).  Historical; `cpld/` is out of scope. |
| `docs/video-enhancements.md` (3), `compositor-overlay-spec.md` (3), `os-design/{01,05}-*.md` (2), `pcbv2-ports-design.md` (1) | 9 | superseded design records that discuss micro-HAM as a candidate.  `compositor-overlay-spec.md` already says it is superseded by `vidcmd-spec.md`. |
| `vidcmd-spec.md` | 1 | the 2026-09-20 addendum listing what was retired.  Correct. |
| `indexed-pixel-spec-draft.md` | 3 | the folded-in draft, excluded by the brief as a draft/notes file; listed for completeness. |
| `microham.txt` (repo root) | — | not matched by the sweep's include list (`.txt`).  It is the owner's original format note; left alone, flagged here as a deletion candidate. |

---

## Gates

| gate | result | evidence |
|---|---|---|
| repo-root `make` + `make check-generated` | PASS | — |
| `super-engine` `make check` | PASS, all cases | — |
| emulator rebuild | clean (debug instrumentation used during the firmware hunt was reverted from the backup) | — |
| firmware build | clean, `rom.bin` 130,508 bytes, 564 free | — |
| `apps/{hello,exctest,basic,dtest,smurf,dmabench}` build | clean | — |
| `make -C image-tools check` | PASS: 9 round trips (K = 0/4/8 x none/ordered/fs) **suite == preview EXACT**, decoder == preview byte for byte, validator rejects a corrupted file, sprite header compiles | — |
| clut-experiment refactor is behaviour-preserving | 4 re-encoded streams byte-identical to the recorded sweep | — |
| `dtest` headless | correct picture (dark-blue field, white square; no paddle input so the bar is zero-width, as designed) | `package3-evidence/shot-dtest.png`, `dtest-console.txt` |
| `smurf` headless | correct picture; 59.79 fps, 0 late frames, 0 packet truncations | `package3-evidence/shot-smurf-title.png`, `smurf-console.txt` |
| smurf backdrop vs the pre-change art | compared by eye against the micro-HAM preview from `HEAD` | `package3-evidence/smurf-meadow-preview-{BEFORE-microham,AFTER-4bpp}.png` |
| `dmabench` check script | PASS C1 76.1 %, C2 55.9 % | `package3-evidence/dmabench-check.txt` |
| `dmabench` full table | all six rows, ratios unchanged | `package3-evidence/dmabench-table.txt`, `shot-dmabench-C1.png`, `shot-dmabench-C3.png` |
| firmware console still boots | correct picture, new help text | `package3-evidence/shot-console-after.png` |
| firmware `view` on a converted reference photo | correct picture **and pixel-exact** against the encoder preview, at K = 0 and K = 4 | `package3-evidence/shot-view-maya2-K{0,4}.png`, `view-pixel-compare.txt`, `view-console.txt` |

**The smurf before/after comparison, stated plainly.**  I did **not** build the
`rev1-video` emulator and app to produce a pre-change screenshot: that is a full
CMake + SDL3 build of another worktree's emulator plus its own firmware and its
own CF image, which is not cheap.  What I compared instead is the thing that
actually changed — the backdrop art.  `image-tools/smurf-assets/backdrop1-meadow-preview.png`
at `HEAD` is what the old app painted (micro-HAM); the new file is what the new
one paints (4bpp half rate).  Side by side they are the same scene: sun, two
clouds, sky gradient, two hills, grass, the brown fence with its gap.  The new
one is visibly coarser horizontally (320 groups doubled) and slightly banded in
the sky.  Everything else in the app — sprites, text, HUD, audio, pacing — is
unchanged code, and the running screenshot confirms it draws.

**The reference photographs were never copied into the repo.**  `clut_encode`
reads `/Users/grantham/trees/griffin-project/reference-images/maya2.ppm`
directly; the `.gc4`, the preview and the CF image built from them are under the
git-ignored `out/` (a new `.gitignore` entry).

---

## Not done

* The griffin-ports Smurf app (`~/trees/griffin-project/griffin-ports`) is a
  separate repository and was not touched; package 2's list named it.
* `board/` is untouched: the palette RAM, the eight extra PIXELS Q traces,
  `IDX`/`nCLUT_WE`, the retired TIMING socket and the five-TAP JTAG chain are
  all still Rev 1 there.  `griffin.md`'s checklist now names the palette RAM.
* The `super-engine/` stale prose in the sweep table.
* The `pixel.v` pin conflict is written up, not resolved (RTL out of scope).
* The firmware local-array finding is characterised, not explained; the 1bpp
  `view IMAGE PALETTE` path builds its packets the same way and was not
  exercised by any gate here.
