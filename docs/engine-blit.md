# ENGINE blit ideas — copy/fill accelerator, transparency writes

Design notes from 2026-07-25 discussion.  Companion to
docs/video-enhancements.md (VIDEO-side modes); this file is the ENGINE-side
counterpart.  Nothing here is committed work.  Motivating use case includes a
homebrew Zaxxon (diagonal-scrolling background + sprites), so the sections
are ordered by likely payoff-per-effort.

## Shared foundation (applies to both sections)

The core observation: write-back DMA does not violate ENGINE's data-blind
design.  A blit FIFO stores the data externally, exactly as the video FIFOs
do:

* **Fill** (RAM → FIFO): ENGINE masters, R_nW=1, strobes FIFO /W — the
  existing streaming read, pointed at a different FIFO.
* **Drain** (FIFO → RAM): ENGINE masters, R_nW=0, asserts FIFO /RE (7200
  outputs drive the bus while /RE low), pulses UDS/LDS as the write strobe.
  Data flows FIFO → bus → SRAM; ENGINE touches only addresses and strobes.

Machinery reused as-is:

* BR/BG/BGACK arbitration and the WORDS_PER_BURST chunking — an op of any
  length is automatically split into short bursts with bus release between,
  which is the anti-starvation policy already proven for video.
* The SETTLE/STROBE 2-cycle loop works for both directions with a mode flag
  selecting which strobe fires (write drains need UDS/LDS pulsed per word
  instead of held, since the SRAM write strobe must edge per address).
* nENGINE_IRQ is routed to GLUE and currently tied inactive — completion
  interrupt is a wire waiting for a purpose.  Poll via ENGINE_STATUS also
  fine.
* Video-refill priority: video HF request wins in STATE_IDLE; blit ops are
  opportunistic.  The video FIFO's half-full margin is ~128 words ≈ 3
  scanlines of slack, so 20-word blit bursts interleave under live video
  without underrun risk.

**The one hard problem — the address counter.**  word_counter runs the video
frame continuously, so blits concurrent with live video need their own
16-bit counter + address mux: ~25–30 additional macrocells on top of 91/128,
landing ~90–95% — the historical grouping-fail zone.  Fallback that fits
trivially: blits share word_counter and are only legal with video disabled
(load-time ops, CF→FB moves, screen transitions).  The dual-counter fit
experiment is the gating question for everything below.

**Width**: an 8-bit FIFO on D[7:0] can only reach the odd byte lane; both
lanes need external drivers and lane logic.  A 16-bit pair of 7200s, exactly
like the video path, is the proven shape — word-granular, full bus width.
Spare 7200s are not in chip-inventory.csv (the video pair was a project
purchase); assume 1–2 more must be bought.

CPU-side model: load address (word_counter or blit counter made
CPU-loadable), load count, kick; wait on IRQ/status; repoint; kick again.
Register decode has spare offsets (A[2:1] uses 2 of 4 slots).

## Section 1: 16-bit-aligned copy accelerator (memmove / scroll / memset)

The high-payoff, low-fiddling one.  Word-aligned, word-granular only.

* **Throughput**: drain writes at 2 cycles/word ≈ 14 MB/s vs the CPU's
  ~2.3 MB/s move.l loop.  Copy = fill + drain double-pass ≈ 7 MB/s net —
  ~3× CPU.  Full 40,320-byte frame move ≈ 6 ms vs ~17 ms CPU.
* **memmove**: fill N words from src (N ≤ FIFO depth 256), repoint, drain N
  to dest.  Overlapping moves work in either direction because the FIFO
  decouples read and write passes — no descending-copy special case needed
  within a chunk (chunk ordering handles overall direction).
* **memset / pattern fill**: two options.
  * Keep a constant-filled RAM buffer (256 words of 0x0000 / 0xFFFF / a
    dither pattern) and repeatedly fill-from-buffer + drain — works with no
    extra hardware.
  * Better: the 7200 has a **/RT retransmit pin** — resets the read pointer
    without erasing contents.  Preload the FIFO once (from RAM or by CPU
    writes), then drain + retransmit repeatedly: constant or 256-word
    pattern fill with *half* the bus traffic (no re-read pass) and no RAM
    buffer.  Costs one ENGINE output pin.  Pattern repeat length = FIFO
    depth = 256 words, plenty for dither/stipple fills.
* **Scroll**: vertical scroll may need none of this — a start-offset
  register in video address generation (the "row stride" idea in project
  notes) scrolls by moving the pointer, zero copies.  The copy engine earns
  its keep on horizontal/diagonal scroll (Zaxxon!), partial-window moves,
  and GUI-style region copies, where the pointer trick doesn't apply.  But prefer using this fill-to-FIFO + fill-from-FIFO for starters.
* Pin cost: /BLT_W, /BLT_RE (+/RT optional) — well within the 15 free I/O.

## Section 2: transparency-bit writes (8/16-bit with skip flag)

The fiddly one; parked here honestly.  The idea: the 7200 is 256×9, and on a
blit FIFO the 9th bit can mean "skip this word" — during drain ENGINE
advances the address but suppresses UDS/LDS.  ~3 product terms.  Combined
with a CPU→FIFO direct-write port (one register address per word, D8 encoded
in an address bit — the PS/2 TX parity trick), the sprite path becomes:

1. CPU shifts/masks sprite data in registers (68000 shifts are cheap),
2. streams prepared words into the FIFO at full bus-write speed with skip
   flags on transparent words,
3. kicks ENGINE to blast it into the framebuffer.

Why it's fiddly / what it is not:

* **No read-modify-write.**  A DMA write clobbers its word; the skip flag
  gives transparency at *word* granularity only — 16 pixels in 1bpp, 4
  pixels in 4bpp@320.  Sprite edges within a word must be pre-merged by the
  CPU (read dest, mask, combine — at which point the CPU has done most of
  the work for those edge words anyway; the win is only the opaque middle).
* Skip granularity could go to byte (UDS/LDS suppressed independently)
  only with a 2-bit flag — but the FIFO pair has exactly one spare bit per
  byte lane (each 7200's bit 8), so per-byte skip is actually expressible:
  EVEN FIFO Q8 gates UDS, ODD FIFO Q8 gates LDS.  Byte granularity = 8 px
  in 1bpp, 2 px in 4bpp@320 — getting usable for Zaxxon-scale sprites.
* Arbitrary bit alignment in hardware (funnel shifter between FIFO and bus
  — '374 + four '157s, or paired '350s) shifts but still cannot merge; by
  the time that's on a breadboard, the right answer is a third ATF1508 as a
  real blitter datapath (shift + mask + logic ops, FIFO as staging).  That
  is Griffin-2000 territory.
* Honest assessment: for Zaxxon-style sprites over a scrolling background,
  the likely-winning split is Section 1 hardware for the background
  scroll/restore (the bulk of the pixels moved per frame) + CPU sprite
  drawing, with Section 2's per-byte skip as a later experiment if sprite
  count becomes the bottleneck.

## Order of operations

1. Fit experiment: dual address counter (+ count register + load paths) in
   ENGINE — the gating question.  If it fails, evaluate the
   video-disabled-only fallback's usefulness.
2. Section 1 only: /BLT_W + /BLT_RE strobes, drain state variant, kick/IRQ
   registers.  Buy 7200s.
3. /RT retransmit for memset/pattern (1 pin, tiny logic).
4. Section 2's skip flag(s) — after Section 1 is proven on hardware.
