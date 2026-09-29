# AUDIO: the Paula-form chip model, the SN76489 channel driver and the Smurf A/B

Step 2 of the plan of 2026-09-27 (griffin.log).  `cpld/audio/audio.v` is the
chip (fits at 120/128); this directory is its host model, which is the
executable spec, plus the two pieces of 68000-side code the chip needs and an
A/B against the software SN76489 in `griffin-ports/smurf/app/audio`.

```
make test      exact tests, then the A/B: build/wav/{reference,audio,audio_stereo}.wav
make ab AB_FLAGS="--burst 4 --margin 3 --seconds 20"
```

## Files

* `audio_chip.h/.cpp` — the model: five 256-byte FIFOs, the 9-bit timebase,
  four period counters, the control reader (4 clocks per byte), the
  rate-multiplier volume gate.  The laws are in the header.  Bare metal
  capable (the emulator will link it).  `AnalogRender` is a listening aid:
  two summing nodes through the output stage's RC corner.
* `scheduler.h/.cpp` — the 68000 side: per-line deposits that keep each FIFO
  fed from a looped table, a mirror of the chip's pop law (`line_pops`, the
  closed form of the counter over one line), PERIOD records landed when the
  old samples have drained, VOLUME records at the next line.
* `frame_scheduler.h/.cpp` — the same laws and the same policy per REGION of R
  lines instead of per line, out of a table indexed by PERIOD, handing out RUNS
  (a pointer into a static ring plus a count) instead of bytes.  "Tier 1" below.
* `psg_channels.h/.cpp` — SN76489 bytes to channel streams, periods and
  volumes, bare metal (2026-09-28): lookups in psg_tables.h, squares as
  windows of one 512-byte buffer, noise from one 32,767-byte table with a
  per-stream repeat; renders the Smurf trace bit-identically to the earlier
  host version.
* `audio_tests.cpp` — exact checks (T1..T9; T8 and T9 are FrameScheduler's,
  and T8 reads the recorded Smurf trace the way `audio_ab` does).
* `psg_tables_gen.cpp` → `psg_tables.h` — the driver's lookups (tone PERIOD/LENGTH per SN76489 period, noise PERIOD/REPEAT per reload, the volume ladder, the white-noise bits), generated on the host and checked in; `make tables-check` proves they match.  `make m68k` compile-checks the 68000 side.
* `audio_ab.cpp` — the replay, statistics and WAVs.
* `tb_vectors.cpp` — a seeded random stimulus through the model, written as
  the vectors `cpld/audio/audio_tb.v` checks the RTL against (`make -C
  ../cpld audio-sim` runs both; step 4, passed 2026-09-27 with all five
  negative controls failing as required).

## Expectations, derived before running

**Chip laws** (from audio.v):
* T1: the six windows are disjoint and tick 0 is in none, so the on-count per
  64-tick frame is exactly VOLUME for all 64 values.
* T2: with COUNT c and PERIOD p at a line's first step, the line (50 steps:
  800 clocks; the first draft of this file said 1600 and 100, corrected
  before the first full run when every note came out an octave high) pops
  `1 + (49 - c) / (p + 1)` times when c < 50 (else 0) and leaves
  `p - (49 - last_pop_step)`; a PERIOD record complete in the control FIFO at
  the line's first clock is written by clock 6 (a second one by clock 14) and
  is in force at that line's first step.
* T3: an empty FIFO forces the gate off; VOLUME 63 is on 63 ticks of 64.

**Scheduler** (burst 8 lines, margin 2):
* T4: a channel at PERIOD p pops 50/(p+1) per line; the burst tops the FIFO
  up to (pops over 8 lines + 2), so the fill never exceeds that plus one and
  never reaches zero; with one record per line the mirror equals the chip
  exactly.  PERIOD 99: 4 + 2 = 6 max fill.  PERIOD 12: 31 + 2 = 33, one
  descriptor.
* T5: a PERIOD change lands within one line of the old samples draining.
* T6: every SN76489 period 15..1024 gets a (PERIOD, LENGTH) within a cent;
  periods 1..14 are band-limited to silence, as the reference does.

**Smurf A/B** (the numbers the report should show):
* Underruns, dropped deposits, dropped control records: 0.
* Mirror fill within the margin (2) of the chip's fill on every line; most
  lines exact.
* Tones stream at 6 to 31 kHz (PERIOD 49..255): 0.2 to 1 word per line per
  channel; noise at 7 kHz (rate 0, PERIOD 224) or up to tone 2's rate.
  Words per frame roughly 3 x 15 kHz / 60 + 7 kHz / 60 = 870; descriptors
  per frame 4 x 525 / 8 = 262 plus a few control records.  ENGINE cost about
  262 x 10 + 870 x 2 = 4,400 SYSCLK of the frame's 233,625 (1.9 %); the worst
  line about 3 descriptors and 12 words = 55 cycles of 445.
* Latency from a PSG write to the speaker: the burst wait (up to 8 lines,
  0.25 ms) plus the fill (pops per burst + 2 samples), under 1 ms.
* Tables: one square table per distinct tone length, a few dozen; two noise
  tables (white 32767 samples, periodic 15 x dup) plus dup variants; under
  100 KB of bytes, twice that as 68000 words.
* Against the reference: equal RMS within 10 % (the RC corner at 9.7 kHz
  takes some energy off the squares' harmonics), correlation well above 0.9
  at a lag of a few ticks; bit identity is not a goal.  The WAV is the gate,
  by ear, as gate 1 was.

## Changes after the first run

* 2026-09-27, later: samples are two's complement again (0x00 = silence; a
  74HC04 inverts bit 7 into the ladder), and the chip re-phases its by-16
  divider from PIXEL's HBLANK every line, so steps are raster-locked (a
  per-step pulse fed in on a pin cost six cells or failed grouping;
  griffin.log).  The model's laws are unchanged, it always assumed that
  alignment; the A/B numbers below reproduce.  Deposits go to the FIFO an
  ENGINE descriptor names by its destination code; the word's high byte is
  ignored.

## Measured (2026-09-27, first full run after the line-length correction)

`make test`: T1..T6 pass.  T4 max fill: PERIOD 99 -> 7, 49 -> 9, 255 -> 4,
12 -> 29 (bound: pops per burst + margin + 1).  T5: the PERIOD record lands
at the third line, 0 old samples at the new rate, 1 new at the old.  T6:
worst tone error 8.15 cents; 42 of the 1010 audible periods exceed a cent,
all high (short LENGTH: (PERIOD+1) x LENGTH has to hit an integer, and 211
is prime).

Smurf A/B, 81.7 s, burst 8, margin 2, RC 9.7 kHz, 2.5 s of host time:

| check | result |
|---|---|
| underruns of an active channel | 0 |
| deposits dropped, control records dropped | 0, 0 |
| mirror fill vs chip | identical on every line |
| max fill per channel | 7, 7, 20, 7 |
| control records | 2,606 (control FIFO max fill 12) |
| tone retargets / noise restarts / noise retargets | 749 / 25 / 462 |
| tables | 33, 1,840 bytes (3,680 as words) |
| descriptors per frame | mean 153, max 268 |
| words per frame | mean 364, max 1,715 |
| worst line | 4 descriptors, 26 words, <= 92 of 445 SYSCLK |
| ENGINE per frame | 2,260 of 233,625 SYSCLK (0.97 %) |
| pitch, the 43 periods Smurf uses | mean 0.94 cents, worst 8.15 (N 15, 7.46 kHz); 10 over a cent, all above 4.6 kHz but one (N 143, 1.24) |
| RMS ours / reference | 0.925 |
| per-field spectral similarity (phase-blind, 256-point) | mean 0.939; 4,019 of 4,510 fields >= 0.9; 21 below 0.5; worst 0.41 at 69.9 s |
| per-field waveform correlation (phase-sensitive) | mean 0.57 at a mean lag of 7.8 ticks: the voices' relative phases differ, as expected |

Against the expectations: words per frame came in at 364, not 870 (Smurf's
tones sit mostly at 6 to 12 kHz rates, and channels are silent much of the
time); descriptors 153 not 262 (silent channels do not burst); ENGINE load
1 %, not 1.9 %.  The 21 fields below 0.5 are not yet characterised; the
guess is transitions during tone 2's sweeps, where our latency of about 8
ticks shifts the sweep by half a field.  Not verified.

The ear's turn: `build/wav/reference.wav` against `build/wav/audio.wav`
(mono mixdown, same scale) and `build/wav/audio_stereo.wav` (tones 0 and 1
left, tone 2 and noise right).

## Tier 1: FrameScheduler, the region table and the rings (2026-09-28)

`Scheduler` costs a 14 MHz 68000 about **1,080,000 SYSCLK a field** — 525
`line()` calls x 4 channels of mirror bookkeeping, plus a byte store per sample —
so the Smurf port's loop ran nine fields behind the raster and eight fields in
nine were missed (`griffin-ports/smurf/app/DESIGN-NATIVE.md`, "What does NOT
work").  `FrameScheduler` is the software-only floor under that: the same laws,
the same policy, paid once per REGION, and not one sample touched by the CPU.

### The contract

Same inputs as `Scheduler`.  `PsgChannels::emit()` is not changed and not
copied: a `FrameScheduler` owns a `Scheduler` as an **inbox**, `emit()` writes
its requests there, `inbox()`'s `line()` is never called, and
`adopt_requests()` reads the inbox by comparing it against the values this class
last wrote into it (an exact edge, because `set_stream` is the only thing that
moves those fields) and publishes the streams' advanced phase back so the next
`emit()`'s `carry_phase()` sees where the streams really are.

Per region of R lines, in this order:

1. **Control first.**  A PERIOD record when every sample that was in the FIFO
   when the change was asked for has been popped (`pending_pops_left`), a VOLUME
   record at the next region.  The per-line scheduler resolves that to a line;
   this resolves it to a region, so a PERIOD lands up to R-1 lines **later** —
   later, never earlier, which is the safe direction, since the rule is "not
   before the old samples have drained".
2. **One top-up per active channel**, to `pops over this region + margin`, a
   queued faster PERIOD assumed for the prediction, capped by the room in the
   FIFO.
3. The region's own pops on the mirror.

A whole region's deposit lands at its **first line**, up to R-1 lines early;
`idle_regions(n)` is `Scheduler::idle_line()` over the same lines (T7).

### The table, and the derivation the code was written from

With COUNT `c` at the first step of a window of S steps and `D = PERIOD + 1`,
the chip pops at step offsets `c, c+D, c+2D, ...`, so

```
pops = floor((S - 1 - c) / D) + 1        count_after = c + pops*D - S
```

Substituting the COMPLEMENT `r = PERIOD - c` collapses both halves into one
division and one remainder **of the same dividend**:

```
pops = floor((S + r) / D)                r' = (S + r) mod D
```

so for a fixed S the whole law is two numbers per PERIOD, and the step is three
instructions:

```
q[PERIOD] = S / D    rem[PERIOD] = S % D        (built once, in reset())
t = r + rem;  pops = q;  if (t >= D) { t -= D; ++pops; }   r' = t
```

exact because `rem < D` and `r < D` put `t` below `2D`.  The two numbers are
**packed into one 32-bit entry** (quotient << 8 | remainder) so the hot path is
one indexed long load, not two.  There is no division, no modulo, no multiply
and nothing wider than an `int` in anything `region()` calls; the tables are
built in `reset()` for the two window lengths a field uses (R lines, and the
last region's).

**Seeding**: `r` IS the chip's COUNT, complemented, so it is seeded from COUNT —
0 at reset (COUNT 0, PERIOD 0) and carried forward as `r'` after that.  Nothing
reads the chip; it cannot be read.

**A PERIOD write does not reset COUNT** (audio_chip.h: COUNT reloads only at a
pop), so a PERIOD record changes D while COUNT keeps counting down.  In
complement form that is a reseed at the same c:

```
r_new = PERIOD_new - c = r_old + (PERIOD_new - PERIOD_old)
```

negative exactly when COUNT is larger than the new PERIOD — `line_pops()`'s
`count >= STEPS_PER_LINE` case, a channel that will not pop for up to 255 steps
after a PERIOD drop.  The window form needs no special case: `r` is normalised
into `[0, D)` by adding D k times and taking k off the window's pop count, which
the identity above makes exact.  With `S >= 300` steps (R >= 6 lines) and COUNT
at most 255, the normalised count can never go negative, so no pop is ever owed
across a region boundary — which is why a short remainder region is merged into
the one before it instead of being left at 5 lines (250 steps).

### The rings, and the one place a word is written

ENGINE moves WORDS and a sample FIFO takes the low byte of each, so a
descriptor's source must be `uint16_t[]` with the samples in the low bytes.  A
**ring** is the caller's byte table expanded once into static word storage, the
pattern repeated until `ring_words >= length + 256`, so a run of at most a
FIFO's worth starting anywhere in `[0, length)` is contiguous: no wrap, one
descriptor per 32 words and no more.  A table too long to duplicate (the
32,767-sample white-noise sequence) is stored once and a run that crosses the end
is split into two.  Rings are keyed by (table pointer, length) — exactly the
identity `psg_channels.h` hands over — built on first use out of one static pool,
never freed.

| rings the full SN76489 range needs | words | bytes |
|---|---:|---:|
| 130 distinct square lengths (2..307), each duplicated past 256 | 48,038 | 96,076 |
| white noise, 32,767 samples, stored once | 32,767 | 65,534 |
| periodic noise, 15 samples, duplicated | 270 | 540 |
| **total measured by T8's sweep** | **80,536** | **161,072** |
| pool as declared (`FRAME_RING_POOL_WORDS`) | 86,016 | 172,032 |
| what the Smurf trace actually builds | 6,763 | 13,526 |

**`repeat > 1` is the exception, and it is staged, not ringed.**
`psg_channels.h` uses `repeat` for a noise shift rate below the chip's 6.15 kHz
floor, where each sequence bit is deposited `repeat` times.  Expanding that into
a ring costs the sequence times the repeat, and `psg_tables.h` produces **57
distinct repeats** over the reload range: **7,453,660 bytes** of word rings,
which is not a design.  Those streams are STAGED — the scheduler writes the
region's words into a window the caller supplies (the region's own payload) and
the run points at it.  It is bounded: the smallest PERIOD among the repeat >= 2
noise entries is 133, so a region of R lines stages at most `50R/134 + 3` words
(26 at R = 64).  Everything else — every tone, and noise at repeat 1 — is pure
reference; over the Smurf trace 2.40 % of samples are staged.

### Region count against FIFO depth

A region's whole deposit lands at its first line, so the FIFO must hold
`50R/(PERIOD+1) + margin + 1`.  The fastest PERIOD `psg_tables.h` can pick is
**13** (noise reload 1), which is the number that matters: a game that drives
noise at rate 3 takes its reload from tone 2's period register, and tone 2
sweeps.

| FIFO | safe R | regions a 525-line field | audio descriptor slots (37 a region) |
|---|---:|---:|---:|
| 7200, 256 | 70 | 9 (R = 64) | 333 |
| 7201, 512 | 142 | 5 (R = 128) | 185 |
| 7202, 1024 | 286 | 3 (R = 256) | 111 |

`min_safe_period()` reports the floor a given configuration has.  R = 176 (three
regions) was built and measured: it is safe only down to PERIOD 34 and it
underran the Smurf playthrough 3,687 times, so the port asserts the floor at
compile time rather than trusting a tone range.

### Measured (2026-09-28)

`make -C audio test` — T1..T7 unchanged, plus:

* **T9**: the region table against `line_pops()` summed over the region, for
  R in {8, 16, 32, 64, 128, 176, 256}, the first and last region of each, all
  256 PERIODs and all 256 COUNTs (which covers the negative-accumulator reseed):
  **917,504 cases, exact** — same pops, same accumulator.
* **T8**, `FrameScheduler` and `Scheduler` each driving a chip model:

| T8 case (R = 64) | result |
|---|---|
| Smurf trace, 4,897 fields | 0 underruns and 0 dropped deposits on both sides |
| samples a field | 362.7 mean (the per-line scheduler's 362.7) |
| **runs a field** | **23.9 mean**, max 10 a region, max 6 a channel |
| staged samples | 2.40 % |
| sweep: every PSG period 1..1023, tone on channels 0 and 2, white and periodic noise at rate 3, 0.26 s each | 0 underruns, 0 drops, **byte streams identical**, **chip output identical on all 1.7 G channel-steps** |
| pre-existing driver defect the sweep found | 128 of the 4,092 cases underrun on BOTH schedulers |

The sweep's **bit-identity** is the strong result: with nothing changing, the
region scheduler and the per-line scheduler feed the chip the same bytes and the
chip's Q bus and gate agree on every step.  Over the Smurf trace they part
company — first at field 34 — because a stream CHANGE (a retarget, a start) lands
where the two mirrors hold slightly different in-flight fills, and
`carry_phase()` then maps the position differently.  The size of that difference
is what is bounded, and it is small:

| Smurf trace, the two schedulers side by side | R = 8 | R = 64 |
|---|---:|---:|
| stream decisions that differed (table, length, repeat, active, per channel per field) | **0** | **0** |
| mirror fill gap, max | 13 samples | 13 samples |
| stream phase gap, max | 23 samples | 23 samples |
| cumulative deposit gap, max | 94 bytes | 109 bytes |

So the two play the same notes from the same tables at the same periods, up to
1.5 ms of phase — which is the same order as the phase difference the A/B already
reports against the software SN76489 ("mean lag 7.8 ticks: the voices' relative
phases differ, as expected").  Bit identity against the per-line scheduler is
not a goal once a stream changes; it is a gate where nothing changes.

**The pre-existing driver defect T8 found.**  `psg_channels.h`'s
`Shadow::period` uses `0xFF` as "no PERIOD yet", which collides with a real
PERIOD of **255**: `emit()`'s `if (tone.period != sh.period)` is then false, no
PERIOD record is ever sent, and the channel plays at the reset PERIOD 0 —
popping 1,573,437 times a second on an empty FIFO.  It hits 12 of the 1,023 tone
periods and 52 of the noise reloads, 128 of the 4,092 swept cases, on BOTH
schedulers.  The Smurf trace never reaches one.  The minimal fix is in
`psg_channels.h`, which this package does not edit: widen `Shadow::period` to
`uint16_t` with a sentinel of `0x1FF`, or add a `period_valid` flag.

### Cost on the 68000 (the reason this exists)

`GRIFFIN_PROFILE` over 12 M SYSCLK (51 fields) inside the Smurf loop,
`replay 300 --native`, audio on, R = 64 (`bench/elf_profile.py`):

| | SYSCLK a field |
|---|---:|
| `FrameScheduler::region` | 32,259 |
| `AudioStream::field` (the port's slot writing) | 27,051 |
| `FrameScheduler::emit_run` | 18,857 |
| `FrameScheduler::advance_region` (idle fields) | 3,340 |
| `FrameScheduler::adopt_requests` | 2,888 |
| `FrameScheduler::bind_ring` | 2,040 |
| `PsgChannels::write` + the queue | ~1,600 |
| **everything audio** | **~88,000 of 233,625 (38 %)** |
| what it replaced | ~1,080,000 (462 %) |

That is a **12x** reduction and it is what makes the loop keep up (2 late fields
of 299 with audio on, against a loop that was nine fields behind).  It is not
"hundreds of SYSCLK", and the profile says why: 9 regions x 4 channels is 36
channel-regions a field, and each one costs about 2,400 SYSCLK across the three
layers — roughly 150 68000 instructions at 16 SYSCLK each, for a table lookup, a
compare, a fill update and three word stores into a descriptor.  Nothing in that
is waste; it is the instruction count of the bookkeeping itself.

So the software floor is about **20,000 SYSCLK a field per region-pass of four
channels**, and the lever is the region count, which is the FIFO's depth:

| FIFO | regions | audio a field, projected from the measured per-channel-region cost |
|---|---:|---:|
| 7200, 256 (measured) | 9 | ~88,000 |
| 7201, 512 | 5 | ~50,000 |
| 7202, 1024 | 3 | ~30,000 |
| 1024 with a 2-region field | 2 | ~20,000 |

Hundreds of SYSCLK a field needs the top-up itself out of the CPU — a counter in
the AUDIO CPLD that walks a ring descriptor, which is a Tier 2 hardware change,
not a software one.
