# COMPOSITOR / OVERLAY — Hardware Specification (draft for fit analysis)

Status: design draft, pre-fit. Target device: ATF1508AS PLCC-84, dedicated
(no CPU bus interface). This document is self-contained; it distills the
overlay design conversation for global resource review. Primary question
for review: **does the register/logic budget fit 128 macrocells, and if
not, which levers to pull.**

---

## 1. Position in the video pipeline

```
ENGINE (descriptor DMA) ──► PIXELS FIFO ──► PIXEL ──► COMPOSITOR ──► VGA DAC
                       └──► OVERLAY FIFO ─────────────────┘
                                     TIMING ──► (hsync/vsync/blank, /RS, pixel clock domain)
```

* COMPOSITOR sits after all pixel generation (1bpp direct and micro-HAM),
  operating purely in the 8-bit R3G3B2 domain. It has **no CPU bus**; all
  configuration arrives in-band through the OVERLAY FIFO.
* The OVERLAY FIFO is filled by one ENGINE DMA descriptor per scanline
  during that line's HBLANK (fixed word count per line), with a multi-line
  priming cushion loaded during VBLANK. TIMING asserts a common FIFO
  reset (`/RS`) to PIXELS and OVERLAY during vertical sync, so both
  streams' line-phase is re-established every frame.
* Rationale for RGB-literal colors (no indexed color / LUT): a 16×8 LUT
  is 128 FFs — the whole device — and index-domain compositing breaks
  over micro-HAM lines, which have no index stream. Palette indirection,
  if wanted, lives in the CPU-side list builder.

## 2. External interface (estimated pin budget)

| group | signals | dir | count |
|---|---|---|---|
| Clock/reset | PIXEL_CLK (25.175 MHz), nRESET | in | 2 |
| Pixel in | RGB_IN[7:0] (R3G3B2 from PIXEL) | in | 8 |
| Pixel out | RGB_OUT[7:0] (to resistor DAC) | out | 8 |
| Overlay FIFO | OVL_Q[15:0] (two IDT7200L-15 in parallel: EVEN = Q[15:8], ODD = Q[7:0], shared /RE) | in | 16 |
| Overlay FIFO control | nOVL_RE | out | 1 |
| Timing | H_ACTIVE (or blank), LINE_START (or hblank-start strobe), V_ACTIVE | in | 2–3 |
| Optional | OVL_Q8 (9th-bit desync detector), ERR out to TIMING status | in/out | 0–2 |

Total ≈ 37–40 of 64 I/O. Pins are not the constraint; macrocells are.

FIFO read timing: IDT7200L-15 read cycle ≈ 25 ns < 39.7 ns pixel period,
so a sustained **1 word per pixel clock** pop rate is available.

## 3. Overlay stream format

All entries are 16-bit words. The per-line record is a **fixed N words**
(N chosen at build time, e.g. 16 or 32), padded with `0x0000`. Fixed word
count per line is structural: it keeps words-consumed-per-line equal to
words-deposited-per-line so OVERLAY stays in lockstep with PIXELS, and it
re-frames the entry decoder at every line start (a malformed entry can
only corrupt its own line).

All counts are stored **complemented** (`~count`): counters load the
complement and count *up* to an all-1s terminal, which is the cheap
terminal detect on ATF15xx (a downcounter's zero detect and reload
constants cost product terms).

### 3.1 Entry types (2-bit type in bits [15:14] of the lead word)

```
type 00 — PASSTHROUGH RUN                                    1 word
  { 2'b00, ~count[13:0] }
  Emit RGB_IN unmodified for count pixels.
  0x0000 (~count = 0) = passthrough 16383 px = "to end of line";
  the all-zero pad word is therefore a valid no-op by construction.

type 01 — SOLID RUN                                          1 word
  { 2'b01, F, ~count[12:0] }
  Emit held color for count pixels. F selects held_fg (0) / held_bg (1).

type 10 — MASKED TILE                                        3 words
  word 0: { 2'b10, ~skip[9:0], flags[3:0] }
  word 1: fg_mask[15:0]        (MSB = leftmost pixel)
  word 2: bg_mask[15:0]
  Passthrough for skip pixels, then 16 mask-controlled pixels:
    fg=1, bg=0  → held_fg
    fg=0, bg=1  → held_bg
    fg=0, bg=0  → passthrough (RGB_IN)
    fg=1, bg=1  → invert: RGB_OUT = ~RGB_IN (XOR — always-visible cursor)
  flags[3:0] reserved (candidate uses: absolute-X mode, mask replicate).

type 11 — SET HELD COLOR                                     1 word
  { 2'b11, F, color[7:0], spare[4:0] }
  Load held_fg (F=0) or held_bg (F=1) with an R3G3B2 literal.
  Zero playback duration (consumes fetch slots only).
```

Every entry except the tile is a single word, so the framing decoder has
no "payload" state for them. Positioning is relative (skip/run counts);
see §6 for the absolute-X variant.

Held colors persist across entries and lines until rewritten; `/RS` at
vsync returns them to a default (suggest fg = 0xFF, bg = 0x00). The
VBLANK preamble in the display list re-establishes them each frame.

### 3.2 Typical costs

* 16-wide 2-color arrow cursor: 3 words per covered line ≈ 96 bytes/frame,
  plus a 2-word color preamble per frame.
* Mono XOR cursor: tile with fg_mask = bg_mask (bits set = invert),
  same 3 words/line.
* Color change between sprites: +1 word per changed register.

## 4. Internal architecture

Two loosely coupled machines in the PIXEL_CLK domain, decoupled by a
one-entry staging register.

### 4.1 Fetch machine (pop side)

Pops one word per clock whenever staging is not full and the line's word
budget (N) is not exhausted.

State:

| register | bits | purpose |
|---|---|---|
| word_pos | 2 | which word of the current entry is being collected |
| cur_type | 2 | latched type of the entry being collected |
| line_word_cnt | 5–6 | words popped this line, drain-to-N enforcement |
| stage_valid | 1 | staging holds a complete entry |

Behavior:

* Lead word: latch type; single-word types complete immediately
  (SET HELD COLOR is applied directly to the held register at pop time —
  it never occupies staging).
* Tile: collect words 1–2 into staged masks, then set stage_valid.
* End of active video: continue popping until line_word_cnt == N
  (**drain-to-N**), discarding, so every line consumes exactly N words
  and framing resets cleanly at the next line.

### 4.2 Staging registers (one decoded entry ahead)

| register | bits |
|---|---|
| stage_type | 2 |
| stage_count / stage_skip | 14 |
| stage_F | 1 |
| stage_fg_mask | 16 |
| stage_bg_mask | 16 |

### 4.3 Playback machine (pixel side)

| register | bits | purpose |
|---|---|---|
| play_state | ~2 | run / skip / tile-active / idle-passthrough |
| run_cnt | 14 | up-counter to all-1s (loaded with ~count / ~skip) |
| fg_shift | 16 | tile fg mask shifter, MSB out |
| bg_shift | 16 | tile bg mask shifter, MSB out |
| held_fg | 8 | overlay color register |
| held_bg | 8 | overlay color register |

Per pixel clock during active video:

```
fg bg (shifter MSBs, tile active) → output mux:
  passthrough : RGB_OUT <= RGB_IN
  fg          : RGB_OUT <= held_fg
  bg          : RGB_OUT <= held_bg
  invert      : RGB_OUT <= ~RGB_IN
```

Entry handoff: when the current entry's terminal is reached (run_cnt all
1s, or 16th tile pixel), the staged entry transfers to the playback
registers on the same edge and its first pixel is emitted on the next
clock — back-to-back tiles (skip = 0) render seamlessly.

Outside active video: output forced to blank level, playback idles,
fetch continues (drain / prefetch).

### 4.4 Line and frame lifecycle

* **VSYNC:** TIMING pulses `/RS` on both FIFOs (while /R and /W idle);
  held colors reset to defaults; all playback/fetch state to idle.
* **VBLANK:** ENGINE display-list preamble loads the priming cushion
  (k lines of overlay records) and the frame's initial SET HELD COLOR
  entries.
* **HBLANK (each line):** framing resets; fetch pops the line's first
  entry into staging (~160 pixel-clock window — the slackest point in
  the line, since PIXELS header pop is a different FIFO/port).
* **Active (640 clocks):** playback consumes; fetch eagerly refills
  staging during each entry's playback.
* **h = 640:** drain remaining words to N.

## 5. Timing invariant (fetch can hide under playback)

Fetch and playback rates make prefetch sufficient by arithmetic:

> **playback_duration(entry k) ≥ word_count(entry k+1)**
> with HBLANK crediting the line's first entry.

A tile plays ≥16 clocks and no entry exceeds 5 words (worst case:
2× SET + 3-word tile), so tiles always cover their successor. Runs
satisfy it whenever count ≥ 5 — and short gaps between sprites should be
transparent mask columns, not runs, so the rule is natural. It is
enforced by the **list builder and emulator** (assert), not by hardware —
same doctrine as the DMA engine's per-line cycle budget: the list is the
schedule.

Defined hardware behavior on violation (underrun): emit passthrough and
hold until staging is valid. With relative skips a late entry slides
right; errors are still bounded to the line by drain-to-N.

## 6. Open options (fit-dependent)

1. **Absolute-X tiles** (flags bit or global): tile word 0 carries
   X[9:0]; playback compares against an internal 10-bit pixel counter
   instead of down-counting a skip. Underrun then clips the late tile
   but every subsequent entry lands at its correct X — errors stay local
   instead of accumulating. Costs a 10-bit position counter + 10-bit
   equality compare; take it if it fits.
2. **Q8 desync detector**: 9th-bit toggle check as in the PIXELS path
   (~2 FFs + 1 pin + sync). With /RS-per-frame it becomes a per-frame
   diagnostic rather than sticky-forever.
3. **Drop bg_mask** (1 mask + fg/transparent only): saves 32 FFs
   (staged + shifter) if the budget fails; loses 2-color tiles and the
   free invert state. Last resort.
4. **Shrink run counters**: counts never exceed 640, so 10 bits suffice
   for playback (14-bit encodings still fine in the format); saves ~4 FFs
   at the cost of not treating 0x0000 as "passthrough to EOL" via count
   alone (would need an explicit EOL state).

## 7. Resource estimate (the concern)

FF budget against 128 macrocells (1 FF each; product terms shared per
macrocell, 5 native + foldback):

| block | FFs |
|---|---|
| Playback: fg_shift + bg_shift | 32 |
| Playback: run_cnt | 14 (or 10, see §6.4) |
| Playback: held_fg + held_bg | 16 |
| Playback: play_state | 2 |
| Staging: masks | 32 |
| Staging: count/skip + F + type + valid | 18 |
| Fetch: word_pos + cur_type + line_word_cnt | 9–10 |
| Output register RGB_OUT | 8 |
| Sync/misc (blank pipeline, RE register, Q8 option) | 4–6 |
| **Total** | **≈ 135–138** |

**This is over budget as drawn — the global review should focus here.**
Known levers, in preference order:

* RGB_OUT FFs may be shareable with the final output mux macrocells
  (the mux result *is* the register input — no separate stage needed): −0
  but check product terms per bit (4-way mux ≈ 4–5 PTs, at the edge).
* run_cnt at 10 bits instead of 14: −4.
* Staged count/skip can be 10 bits (skip ≤ 639): −4.
* Merge staged skip counter with playback run_cnt if handoff timing
  allows (skip counts down *before* masks activate, so the staged copy
  may be unnecessary — playback could run the skip while staging holds
  only masks): −10 to −14. Most promising single lever; needs a careful
  look at back-to-back handoff.
* Single-mask fallback (§6.3): −32. Changes the feature set; last resort.

Product-term risks to check in fit: the 4-way per-bit output mux, the
14-bit all-1s terminal detect (single wide AND — cheap), staging-to-
playback parallel load enables (keep single-literal enables, per the
PIXEL/HAM lesson), and the drain-to-N compare.

## 8. Explicit non-goals

* No CPU bus interface, ever — configuration is in-band only.
* No indexed color / LUT in this device (see §1 rationale).
* No inter-line state other than held_fg/held_bg.
* No overlapping tiles within a line (X is monotonic by construction);
  a cursor needs one tile per line.
* Overlay applies after micro-HAM on final RGB; it must remain agnostic
  to the pixel mode that produced RGB_IN.
