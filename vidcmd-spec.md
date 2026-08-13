# PIXEL + COMPOSITOR / VIDCMD — Combined Design

Status: design record, pre-fit.  Captures the architecture converged on in
design discussion 2026-08-07/08.  **Supersedes compositor-overlay-spec.md**
(fixed-N framing, 8-bit color, zero-duration SET records are all replaced
here).  Timing skeleton and measured-constant testbench:
`super-engine/set-timing.v`.

---

## 1. Architecture

```
ENGINE (descriptor DMA) ──► PIXELS FIFO ──► PIXEL ──► COMPOSITOR ──► 12-bit DAC
                       └──► VIDCMD FIFO ──────┬───────────┘
                               (Q[11:0] also tapped by PIXEL)
                     TIMING ──► PIXEL_CLK, LINE_START, H_ACTIVE, /RS
```

Two FIFOs decouple ENGINE's DMA from the video pipeline.  Neither PIXEL
nor COMPOSITOR has a CPU bus interface; all data and configuration arrive
in-band.  Everything runs in the shared 25.175 MHz PIXEL_CLK domain.

* **PIXELS is the data plane**: high rate, fixed length per line per mode,
  no semantics beyond pixel bits.  No in-band header — the old 4-byte
  palette/mode header is retired; those values arrive as VIDCMD SETs.
* **VIDCMD is the control plane**: variable length per line,
  position-synchronized instructions that composite over the picture and
  write registers in both chips.  It is the out-of-band register-write
  path: PIXEL needs no bus port and ENGINE needs no register-write DMA op.

Color is R4G4B4 (12-bit) throughout: PIXEL registers, the inter-chip RGB
bus, COMPOSITOR held colors, and the resistor DAC (three 4-bit ladders;
LSB ≈ 47 mV at VGA levels, 1% resistors adequate).

## 2. PIXELS stream

| mode | rate | words/line | meaning |
|---|---|---|---|
| 1bpp direct | 1 bit/clock | 40 | bit selects pal_fg / pal_bg |
| 2bpp micro-HAM | 2 bits/clock | 80 | dibit codes, below |

Micro-HAM codes are unchanged from the 1bpp design, just consumed at
2 bits/clock (so each op spans half as many pixels):

```
0_p     1 clk   held <- pal[p]           (p: 1 = fg, 0 = bg)
10_g_r  2 clk   held green <- g, held red  <- r   (1-bit, replicated to 4)
11_g_b  2 clk   held green <- g, held blue <- b
```

Line start: held <- pal_fg.  No cross-line state.  Full-precision colors
enter via VIDCMD SET (pal_fg, pal_bg, ham_held), not via HAM codes —
that is why the code space stays this small.

## 3. VIDCMD instruction set

All words 16 bits.  Counts stored complemented (`~count`): playback
counters load the complement and count up to the all-1s terminal.

```
RUN   { 2'b00, src[1:0], ~count[11:0] }                      1 word
      For count pixels: src 00 = passthrough (RGB_IN),
      01 = held_fg, 10 = held_bg, 11 = spare
      (candidate: invert, RGB_OUT = ~RGB_IN).

TILE  { 2'b01, ~skip[9:0], flags[3:0] }                      3 words
      word 1 = fg/bg select mask (bit 1 -> held_fg, 0 -> held_bg)
      word 2 = opacity mask      (bit 1 -> overlay,  0 -> passthrough)
      MSB = leftmost.  Passthrough for skip pixels, then 16
      mask-controlled pixels.  flags reserved.

SET   { 1'b1, target[2:0], value[11:0] }                     1 word
      target 0 = cmp_held_fg     4 = pix_ham_held
             1 = cmp_held_bg     5 = pix_mode
             2 = pix_pal_fg      6 = pix_pixel_skip
             3 = pix_pal_bg      7 = spare
```

Held colors persist until rewritten; `/RS` at vsync resets them to
fg = 0xFFF, bg = 0x000 and clears all machine state.

## 4. Execution model

### 4.1 During active video (H_ACTIVE)

* One slot per clock.  RUN and TILE consume their durations.
* **A SET occupies one slot and commits on the clock edge that begins
  it** (the handoff edge — its payload has been parked in registers since
  pop, so this is the same registered parallel-load as a run-counter
  load).  During its slot the output continues the previous mode, under
  the new state.
* Therefore: **a SET preceded by playback records totaling N slots takes
  effect starting at pixel N** (plus the constant per-target screen
  offset K, §6).  Every boundary is reachable; consecutive SETs land on
  consecutive pixels.  `...passthrough, SET, passthrough...` renders as
  one unbroken passthrough whose state changes exactly on the boundary.

### 4.2 Outside active video

* Playback idles.  Fetch pops whenever the FIFO has data (gated by the
  synchronized /EF): SETs execute immediately at pop; the first RUN or
  TILE popped parks in staging and waits for H_ACTIVE, releasing at
  pixel 0 with the program one entry ahead.
* Hblank SETs are therefore free (zero slots) and are guaranteed applied
  before the next line's pixel 0, in stream order.  Their exact clock
  depends on ENGINE's deposit timing — the emulator asserts the weak
  property (before line start) for blank SETs and the strong pixel-exact
  property for active SETs.
* No "wait" bits exist or are needed: RUN/TILE are the visible timeline
  and the staged-record release is the only beam synchronization.
  ENGINE descriptor waits shape bus traffic only; the FIFO decouples
  deposit time from effect time.

### 4.3 Line framing and errors

* **Each line's playback records must sum to exactly 640 slots.**
  Minimum program for an untouched line: one word (RUN passthrough 640).
* On starvation (FIFO empty mid-line) playback holds the current output
  mode.  Note: with a VBLANK priming cushion the FIFO is normally never
  empty, so hold-on-empty is a degradation behavior, not a compression
  device — lines must carry their explicit 640.
* Framing violations desync at worst until vsync: **errors are
  frame-bounded**, recovered by `/RS`.  The builder and emulator enforce
  the invariants; the silicon just runs.
* Prefetch invariant (builder-enforced):
  playback_duration(entry k) >= word_count(entry k+1), with hblank
  crediting the line's first entry.  SETs count as duration 1 / word 1.

## 5. Cross-chip mechanism (the SET conduit)

PIXEL taps the VIDCMD Q bus rather than having a second data port:

* **shadow**: PIXEL latches Q[11:0] on every pop (enable = observed /RE).
* **pending**: when COMPOSITOR's registered PIX_TVALID/PIX_TSEL flag the
  previous pop as a PIXEL-target SET, PIXEL captures shadow -> pending
  (value + target).  Same-edge shadow overwrite is safe: pending takes
  the pre-edge value (ordinary pipeline semantics).
* **commit**: COMPOSITOR pulses PIX_COMMIT (registered) and PIXEL loads
  pending -> addressed register.
* Single pending is safe by construction: a second PIXEL-target SET
  cannot pop until the first leaves staging, which happens on its commit
  edge.  Back-to-back eager SETs pipeline at one word/clock
  (shadow -> pending -> register, three stages).

Every cross-chip signal is FF-driven and FF-sampled; there are no
combinational paths through package boundaries.  The one async input,
/EF (deassert edge caused by ENGINE-domain writes), gets a 2-FF
synchronizer in COMPOSITOR.

## 6. Timing constants

Pipeline depth (PIXEL register stage, COMPOSITOR output register, the
one-cycle registered PIX_COMMIT) gives each SET target a small constant
screen offset **K**.  K is *measured*, not derived: the
`super-engine/set-timing.v` testbench prints it, the RTL testbenches
assert it, the emulator asserts the same value, and the list builder
folds it in (equivalently: PIXEL-target SETs are placed a fixed one-ish
slot earlier than compositor-target SETs for the same screen boundary).
Semantics stay pixel-exact; K never varies at runtime.

Electrical budget at 39.7 ns (worst paths, -15 parts):

| path | ns (approx) |
|---|---|
| /RE (tCO) -> IDT7200 tA -> Q setup at both chips | ~35 (pre-existing, unchanged) |
| registered control pins chip-to-chip (tCO + flight + tSU) | ~22 |

Board notes: series-terminate /RE (rev-1 ringing lesson — PIXEL now
samples this net); PIXEL_CLK skew between chips < ~2-3 ns (same net,
matched routing).

## 7. Chip summaries

### PIXEL

Drains PIXELS at 1 or 2 bits/clock; resolves each pixel to R4G4B4 from
its register file; outputs one color/clock to COMPOSITOR.  Decodes no
instructions.

Registers: pal_fg, pal_bg, ham_held (12 each), mode, pixel_skip, shadow
(12), pending value+target (15), HAM decode FSM, stream shifter.

Pins: PIXELS Q[15:0] in, RGB[11:0] out, VIDCMD Q[11:0] in, /RE sense,
PIX_TSEL[2:0] + PIX_TVALID + PIX_COMMIT in, clocks/timing.  ≈ 48 of 64.

### COMPOSITOR

Executes the VIDCMD program in lockstep with scanout; drives the DAC.

Registers (deltas vs. the superseded spec): held_fg/bg now 24 total,
RGB_OUT 12; staging masks double as SET payload parking; line_word_cnt
and drain-to-N deleted; +2 /EF sync, +pending-target bookkeeping.
Estimate remains ≈140 against 128 macrocells before levers
(10-bit playback counters, staged-skip/run_cnt merge); **the fit
experiment is the go/no-go gate**, with single-mask tiles as last-resort
lever.  Handoff-edge loads are PT-dense: keep single-literal enables
(the PIXEL/HAM lesson).

Pins: VIDCMD Q[15:0] in + /RE out + /EF in, RGB_IN[11:0], RGB_OUT[11:0],
PIX_TSEL/TVALID/COMMIT out, timing in.  ≈ 50 of 64.

## 8. Signal table

| signals | path | function |
|---|---|---|
| RGB[11:0] | PIXEL -> COMPOSITOR | composited input, one color/clock |
| Q[15:0] | VIDCMD FIFO -> COMPOSITOR (Q[11:0] also -> PIXEL) | instruction word |
| /RE | COMPOSITOR -> FIFO, sensed by PIXEL | pop; PIXEL shadow enable |
| /EF | FIFO -> COMPOSITOR | empty flag (2-FF sync'd) |
| PIX_TSEL[2:0], PIX_TVALID | COMPOSITOR -> PIXEL | "shadowed word is yours, register N" |
| PIX_COMMIT | COMPOSITOR -> PIXEL | apply pending at the slot boundary |
| RGB_OUT[11:0] | COMPOSITOR -> DAC | final color |
| PIXEL_CLK, LINE_START, H_ACTIVE, /RS | TIMING -> all | clock, epoch, blanking gate, per-frame reset |

## 9. What 12-bit color costs elsewhere (summary)

* Zero memory bandwidth: pixel streams stay 1/2 bpp indexed; literals
  ride SETs and HAM palette entries.
* ENGINE: no change (word streamer).  Display-list builder writes wider
  literals; VBLANK preamble re-establishes held colors and palettes.
* Board: 12 DAC pins + three 4-bit ladders; PIXEL->COMPOSITOR bus is 12
  traces; new-board items alongside the COMPOSITOR/PIXEL chips.
* Emulator/tools: 4-bit channel expansion (c*17), SET/target decode,
  2bpp HAM, r4g4b4 quantizer; super-engine host suite re-run (word
  counts unchanged).

## 10. Open items

1. COMPOSITOR fit experiment at 12 bits with counter-shrink and
   skip-merge levers designed in (go/no-go for the whole design).
2. PIXEL fit (4-entry palette question is moot — two palette entries;
   HAM decode + shadow/pending are the adds).
3. RUN src=11: adopt invert-run or leave spare.
4. TILE flags: absolute-X option (from superseded spec §6.1) still a
   candidate if it fits.
5. Pin down K constants from real RTL; propagate to builder + emulator.
6. VIDCMD FIFO depth / priming-cushion sizing vs. ENGINE descriptor
   schedule (bandwidth arithmetic, per-line word budgets).
