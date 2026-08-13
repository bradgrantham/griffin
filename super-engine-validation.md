# Super-engine use cases distilled to unique validation cases

2026-08-06.  Companion to super-engine.md.  Baseline hardware under validation is
cpld/engine/edma3.v (fits Rev-1 pinout: 123/128 LC, 414 PT): 15-bit descriptor
pointer into RAM's top 64K, 4-word-aligned descriptors, 22-bit source, 5-bit
0-biased count (1..32 words), 7 one-hot deposit strobes, wait-on-HBLANK,
stop+IRQ, bus released between descriptors, DTACK-less 2-cycle streaming.

## Deduplication

The eight scenarios in super-engine.md exercise only six distinct mechanisms.
Each mechanism needs validating once; the scenarios then become parameter sets.

| # | mechanism | covers scenarios | uniquely validates |
|---|---|---|---|
| M1 | Line-paced streaming to one FIFO from an arbitrary word address, lines longer than one descriptor (40 words = 2 x 20, 2bpp 80 = 4 x 20) | 1bpp vertical scroll; 2bpp variants | chained descriptors within one line window; arbitrary source; count-5 chaining overhead |
| M2 | Out-of-band per-line register deposits ordered before the line's pixel DMA: PALETTE (fg+bg in one word), MODE/pixel_skip | per-line colors; horizontal scroll (same mechanism, different values + start addresses) | ordering guarantee reg-writes-before-pixels; deposits landing inside HBLANK |
| M3 | Second FIFO destination inside the same line window (OVERLAY span lists) | cursor (1 span); 4 sprites/line (worst case) | destination switching mid-window; per-line overlay budget |
| M4 | Sparse periodic non-video destination woven into the same list (AUDIO bursts every Nth line) | audio with/without | cadence mapping sample-rate to line-rate; audio jitter bound |
| M5 | Frame handshake: STOP+IRQ at list end, VBLANK ISR re-arms; double-buffered tables | "wait last VBLANK" preamble; whole-frame scroll updates | ISR-re-arm replaces the VBLANK wait codes absent from baseline; author-while-consuming safety |
| M6 | (NOT in baseline hardware) VBLANK mem-to-mem copies and FIFO reset strobes | vblank copies; the _RS write codes | validated only at descriptor-stream level, to decide whether write cycles / RS strobes earn their macrocells (~5 MC headroom) |

The "crazy demo" is M1+M2+M3+M4+M5 composed - it is the capstone parameter
set, not a new mechanism.

## What validation must answer, per mechanism

1. **Expressiveness** - do the baseline descriptor fields encode it at all?
   (Known gaps: no VBLANK wait codes -> M5's ISR re-arm; no write cycles or RS
   strobes -> M6 excluded; palette must be out-of-band, which the deposit model
   provides.)
2. **Table size** - descriptors x 8 bytes vs the 64K (or 32K) window, for the
   worst-case frame.
3. **Bus budget** - per-line cycles vs the line, and how far pixel DMA spills
   past HBLANK into the FIFO cushion.  Model parameters (defaults, 14 MHz
   SYSCLK, VGA 640x480@60):
   - line = ~445 SYSCLK, HBLANK = ~89 SYSCLK
   - per-descriptor overhead = re-arbitration (BR->BG->BGACK, CPU-dependent,
     model as parameter, ~4..10) + ASSERT + 4-word fetch (8) + RELEASE
     = ~14..20 SYSCLK
   - payload = 2 SYSCLK/word
   - example, M2 line = 4 descriptors (palette, mode/skip, 2 x 20 pixels):
     ~4x16 + 42x2 = ~148 SYSCLK = 33% of the line, of which the register
     deposits complete ~35 cycles after the HBLANK edge (well inside HBLANK).
4. **FIFO conservation** - line-clocked occupancy never exceeds depth (2 x 7200
   = 256 words on PIXELS) and never underruns scanout; overlay FIFO likewise.
5. **CPU cost** - cycles for the VBLANK ISR re-arm and for authoring/patching
   the next table (M5 double-buffering; scroll = patch source addresses only).

## Validation vehicle: C++ descriptor authoring + host-side checker

Yes - C++ functions that build the frame descriptor arrays are the right
artifact, with one addition: a small host-side interpreter to run them against.
Neither alone answers the questions above.

- **Authoring functions** (one per mechanism, the crazy demo composing them):
  bare-metal-capable C++23, no I/O, writing into a caller-provided
  std::span<uint16_t> - so the identical code compiles for the host tests today
  and the 68000 firmware later (same doctrine as apps/lib: make the platform
  match, keep the logic verbatim).  Descriptor field encoding lives in one
  shared header; once the format stabilizes it moves into griffin.yml and
  codegen so firmware, emulator, and Verilog agree by construction.
- **Host checker** (~150 lines): interprets a table with edma3.v's exact
  semantics and emits an event timeline (cycle, signal, address, word) while
  asserting: alignment and window bounds, count/field ranges, reg-deposits
  precede pixel payload per line, per-line cycle budget, FIFO occupancy
  bounds, IRQ raised exactly once at list end.  Parameters: arbitration cost,
  line/HBLANK geometry, FIFO depths.
- Ladder after host validation: teach the emulator ENGINE the descriptor model
  and run the same authoring functions from firmware; then a Verilog testbench
  driving edma3.v with a table image; then hardware.  The authoring functions
  are the constant across all four rungs - that is what makes them worth
  writing first.

M6 runs through the authoring functions and checker only (behind a
feature flag), producing the cycle-budget evidence for deciding whether write
cycles and RS strobes justify spending the last ~5 macrocells or belong
elsewhere.
