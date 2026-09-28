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
* `psg_channels.h/.cpp` — SN76489 bytes to channel tables, periods and
  volumes: square tables chosen per tone period, LFSR sequence tables for
  noise, the reference's band limit and 2 dB ladder.
* `audio_tests.cpp` — exact checks (T1..T6 below).
* `audio_ab.cpp` — the replay, statistics and WAVs.

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
