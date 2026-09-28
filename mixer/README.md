# MIXER: the shared model and the SN76489 A/B

`wave-table-design.md`, cited throughout this directory, lives on branch
`z80-audio` since 2026-09-28 (`git show z80-audio:wave-table-design.md`).

Step 1 of `../wave-table-design.md` §3.  The pinned semantics are §2 of that
file; this directory is a host model of them, the PSG-bytes-to-SETs driver the
68000 app will run, and an A/B tool that plays the recorded Smurf PSG stream
through both the software SN76489 (`griffin-ports/smurf/app/audio`, per-sample
path) and this model.

```
make            build build/mixer_ab
make test       run it: build/wav/{reference,mixer}.wav, build/pitch_table.txt, report on stdout
make test-peak60   the same at driver (and reference) peak 60, under build/peak60/; the default stays 31
make PSG_DIR=... TRACE=...   the reference sources and the recording (defaults are the sibling checkout)
```

## Files

* `mixer.h`, `mixer.cpp` — the model: AUDCMD queue (WAIT/SET, bounded
  records per boundary), SAMPLES FIFO passthrough, up to 15 voices of 8 bytes
  plus the MIXER control block (0x78 = LFSR voice select) in the 128-byte SET
  window, square and noise modes (wavetable mode is a stub), one SN76489AN
  LFSR, two saturating 8-bit accumulators (bit-identical to the hardware's one
  accumulator in two passes), `tick()` -> one `{L, R}`.  Bare-metal
  constraints as `super-engine/render.h`: no allocation, no I/O, no exceptions,
  `<cstdint>`/`<cstddef>`/`<array>` only.  The laws are in the header comment.
* `psg_driver.h`, `psg_driver.cpp` — SN76489 port bytes in, SET words out.
  `consteval` INC table (1024 entries, indexed by the raw 10-bit period
  register) and AMP ladder (16 entries), no run-time division.
* `mixer_ab.cpp` — the A/B tool.  Reads the trace with the reference's own
  `tests/trace_io.h`, replays it field by field exactly as `frame_audio.cpp`
  does, and compares.

## The laws, in brief

The full statement is the header comment of `mixer.h` (and of `psg_driver.h`
for the driver); `../wave-table-design.md` §2 is the source, and the points
marked "decided during step 1" there were settled while writing this model.

* One `tick()` per AUDIO_TICK: (1) AUDCMD at the boundary — the WAIT counter
  counts down, and at zero SETs execute until a nonzero WAIT or an empty
  queue; (2) a SAMPLES word seeds the two accumulators (0 if the FIFO is
  empty); (3) voices in index order: `phase += inc`, then the sample from the
  NEW phase (square `phase[15]`, noise `lfsr[0]` as it stood when the tick
  began), added as ±AMP into each PAN-enabled channel, saturating on every
  add; the LFSR voice's carry is latched; (4) `{L, R}` out, then the LFSR
  shifts once if the carry was latched (amended 2026-09-26: a shift is heard
  one tick after its carry, and every voice in a tick reads one value).
* WAIT n consumed at tick k: ticks k .. k+n-1 play, the next record is taken
  at tick k+n.  An empty queue changes nothing.  At most
  `MAX_RECORDS_PER_BOUNDARY` (64, PROVISIONAL until rung (ii) measures the
  idle gap) records are consumed at one boundary; the rest slide a tick.
* MODE 0 square, 1 white noise, 2 wavetable (silent stub), 3 periodic
  noise.  The LFSR (15 bits, taps 0 and 1 into bit 14) shifts on the carry of
  the LFSR voice when that voice is in MODE 1 or 3.  The LFSR voice is SET
  address 0x78 bits 3:0 (15, or any voice not played, selects none); it is
  unreset SRAM, so the driver SETs it.
* SET window: voices 0..14 at voice*8, 0x78..0x7F MIXER control (0x78 LFSR
  voice select, the rest reserved).  Hardware-private state (the WAIT
  counter as an SRAM field, wave pages) lives at SRAM >= 0x80, outside the
  window and outside the model.
* RESET_PHASE (CTL bit 4) zeroes the phase when the SET executes, is not
  stored, and on the LFSR voice reseeds the LFSR to 0x4000.
* Power-on: voice RAM is seeded garbage; /RS clears the WAIT counter, seeds
  the LFSR and empties the FIFOs.  Whoever owns the chip silences the voices
  it does not drive (`silence_voices()`).
* Driver: first emit SETs the LFSR select to 3 before anything else; tone c
  -> voice c, noise -> voice 3; INC from a 1024-entry table
  (mod 65536 below period 8, exact alias), noise INC clamped to 0xFFFF below
  period 8; AMP from the reference's 2 dB ladder at peak 31; a SET only for a
  byte that changed, plus RESET_PHASE on every noise control write.

## Expectations, derived from §2 before the first run

Written 2026-09-26 before `mixer_ab` was run.  Arithmetic only: fs =
25175000/1600 = 15734.375 Hz, PSG clock 3579545 Hz, f(N) = 3579545/(32 N),
INC(N) = round(65536 f(N) / fs), reference STEP(N) = round(2 * 65536 f(N) / fs)
(half-periods, 16.16), peak 31.

| # | quantity | expectation | why |
|---|---|---|---|
| E1 | worst pitch error, all periods | <= 1.90 cents, at or near period 1024 | INC rounding is at most half a quantum, fs/2^17 = 0.120 Hz; at 109.24 Hz that is 1.90 cents |
| E2 | mean pitch error | about 0.48 cents | mean of 0.060 Hz (uniform rounding) over f(N), N = 1..1024 |
| E3 | periods 1..7 | INC wraps (true INC > 65535); the square is the same alias the reference produces | phase[15] depends only on phase mod 2^16, so INC mod 2^16 is exact for a square |
| E4 | AMP ladder | every step within 0.5 LSB of 31 x 10^(-a/10); dB error <= 0.29 dB for a = 0..10; a = 11..14 are 1.8 to 2.2 dB off; a = 12 and 13 BOTH round to 2 | 8-bit AMP at peak 31: at the bottom one LSB is a 2 dB step |
| E5 | tone phase identity | ref.phase ≡ 2 mix.phase + Σ(STEP − 2 INC) (mod 2^17), every tick, all three tones: 0 violations | both are point-sampled phase accumulators started at 0; only the increments differ |
| E6 | noise shift accounting | (ref shifts − mix shifts) x 65536 + (ref frac − mix phase) = Σ(noise_step − INC) since the reseed: 0 violations. Amended: see "Amendment, 2026-09-26". | same argument; the reference keeps the fraction, the MIXER the 16-bit phase |
| E7 | LFSR sequence | at equal shift counts inside a reseed epoch the two registers are equal: 0 mismatches. Amended: see "Amendment, 2026-09-26". | §2 names the reference's register; reseed is RESET_PHASE on the LFSR voice |
| E8 | noise rates 0x10/0x20/0x40 | INC == noise_step (29120, 14560, 7280): identical shift instants. Amended: see "Amendment, 2026-09-26". | the two roundings coincide for these three reloads |
| E9 | noise rate 3 at tone2 period < 8 | MIXER shifts at most once a tick (INC clamped to 0xFFFF); the reference shifts up to about 1.8 a tick.  A real deviation, not rounding | a 16-bit INC has one carry per tick at most |
| E10 | samples where every audible source's sign agrees | abs difference <= 2 LSB | up to four AMP roundings of <= 0.5 each, plus the reference's own final rounding |
| E11 | samples where a sign disagrees | explained entirely by E5/E6 drift; a voice's sign disagreement costs 2 x AMP | with STEP − 2 INC = ±1 on about half the periods, a sustained note drifts to anti-phase in 2^16 ticks = 4.2 s |
| E12 | whole-trace max abs difference | large: at least 62 (2 x AMP[0]) is reachable, at most 248 | follows from E11; the sample domain is not a fidelity measure between free-running oscillators |
| E13 | whole-trace RMS difference | the same order as the signal's own RMS (reference: 21.4 LSB), not small | E11 again; the reference README found the same for the rig |
| E14 | fraction of differing samples | most audible samples (over half of the trace) | E4: every attenuation other than 0 and 15 has a nonzero AMP rounding, so a sign-agreeing sample still usually differs by 1 |
| E15 | 128-sample RMS envelope difference | mean about 1 LSB or less; the jump (E9) the worst | loudness is what E4 fixes; phase drift does not change level |
| E16 | saturation, L vs R | 0 saturating adds; L == R on every tick | 4 x 31 = 124 < 127, PCM input 0, all four voices PAN_L+PAN_R |
| E17 | SETs per tick | <= 13 after the first (3 tones x INC_LO, INC_HI, AMP + noise x 4); the first flush 16; typically well under a dozen. Amended: see "Amendment, 2026-09-26". | driver emits only changed bytes |
| E18 | AUDCMD words if a whole game frame is delivered at once | the death frame (62 groups) needs several hundred words: more than a 256-word 7200 pair | 62 x (SETs + 1 WAIT) |
| E19 | sample count | 1,285,463, identical in both | the reference README's whole-run count, same field alternation |

## Results, 2026-09-26

`make test` on `griffin-ports/smurf/work/smurf-trace.io` (4697 frames, 42,409
PSG bytes, 4897 field groups).  All 12 checks pass.  Expectations are the
table above, unedited.  These are the STEP-1 law's numbers; the amended
model's are in "Amendment, 2026-09-26" below, next to them.

| # | expectation | measured | verdict |
|---|---|---|---|
| E1 | worst pitch error <= 1.90 cents | 1.824 cents, period 1014 (110.3 Hz); ten worst all at periods 890..1023 | held |
| E2 | mean about 0.48 cents | 0.477 cents (the reference's own 16.16 half-period step: max 0.896, mean 0.236) | held |
| E3 | periods 1..7 wrap, exact alias | stored INC == true INC mod 65536 on all 1024 periods | held |
| E4 | ladder within 0.5 LSB; a11..a14 about 2 dB off; a12 == a13 | worst 0.463 LSB (a3); a11 -1.81 dB, a13 +2.19 dB, a14 -1.83 dB; a12 = a13 = 2 | held |
| E5 | tone phase identity, 0 violations | 0 over 1,285,463 ticks x 3 tones | held |
| E6 | noise shift accounting, 0 violations | 0 | held |
| E7 | LFSR on the same sequence, 0 mismatches | 0 over 51 reseed epochs; also 0 on a synthetic stream of all four white rates (the recording has no white noise at all) | held |
| E8 | rates 0x10/0x20/0x40 identical instants | rounding-only shift drift at most 0.348 of a shift in any epoch (that is rate 3); 0.000 on the synthetic rates 0-2 | held |
| E9 | rate 3 at tone2 period < 8 clamps to one shift a tick | 54 fields (the jump sweep tail); shift-count gap up to 221 in one epoch | held; a real deviation (see below) |
| E10 | sign-agreeing samples differ by <= 2 | max 1 LSB, RMS 0.36, over 319,720 samples | held |
| E11 | every sign disagreement is E5/E6 drift | the identities hold on every tick, so yes; first disagreement 0.83 s in (.io frame 47) | held |
| E12 | max abs difference in 62..248 | 176 | held |
| E13 | RMS difference of the order of the signal RMS | 26.86 against a signal RMS of 20.90 (ref) / 20.93 (mixer) | held |
| E14 | over half the samples differ, mostly by amplitude rounding | 73.8% differ, but NOT for the reason given: only 13.0% of sign-agreeing samples differ | number held, reasoning wrong (see below) |
| E15 | envelope difference mean <= ~1 LSB, worst at the jump | mean 0.89 LSB; worst 18.48 LSB, at 19.80 s, NOT the jump: tones 0 and 2 in unison at period 85 | mean held, worst-case reasoning wrong (see below) |
| E16 | 0 saturations, L == R | 0 and 0 | held |
| E17 | first emit 16 SETs, later <= 13, typically under a dozen | 16; max 12; mean 0.93 SETs a group | held |
| E18 | a whole death frame is several hundred words, over 256 | 91 words (frame 874, 62 groups) | WRONG: see below |
| E19 | 1,285,463 samples | 1,285,463 | held |

Output levels: reference RMS 20.90, MIXER 20.93 LSB over the whole run
(38.17 / 38.24 where the noise channel is audible).

### Where the expectations missed, and which side was wrong

None of the three misses is a model defect; each is a wrong derivation, and
the invariants E5-E7 are what rule the model out as the cause.

* **E14.**  I reasoned that amplitude rounding alone would make most sign-
  agreeing samples differ by 1.  It does not: with ONE audible voice both
  models round the same exact value (31 x 10^(-a/10)) to the same integer, so
  they agree.  A difference needs two or more audible voices whose rounding
  errors add past a half LSB, which is 13% of sign-agreeing samples.  The 73.8%
  is almost all phase drift (E11).
* **E15.**  "Phase drift does not change level" is false for voices in unison
  or at octaves.  At 19.80 s tones 0 and 2 both sit at period 85 (atten 2 and
  4, AMP 20 and 12): in the reference their frozen relative phase happens to be
  opposite (RMS 14.17 ~ 20 - 12 with tone 1's octave), in the MIXER it is
  aligned (32.65 ~ 20 + 12).  Each is a legitimate result of a different
  rounding history; the real SN76489's relative phase in that passage is
  whatever its counters happened to hold, and neither model knows it.  Mean
  loudness still agrees to 0.89 LSB.
* **E18.**  I estimated about five SETs a death tick.  The BIOS re-emits every
  register every tick, and the driver sends only bytes that changed, so the
  62-group death frame is 91 words (62 WAITs + 29 SETs).  It fits a 256-word
  AUDCMD pair with room.  (Without diff emission it would be 62 x 17 = 1054.)

### The one real deviation: noise faster than the tick (E9)

Rate 3 clocks the noise from tone 2's period.  S11's jump sweeps tone 2 down to
period 4, where the SN76489 shifts 27,965 times a second, 1.8 a sample.  A
16-bit INC carries at most once a tick, so the MIXER's noise voice tops out at
one shift a tick; `psg_driver` stores 0xFFFF there (and not INC mod 65536,
which would throw carries away).  Over the 54 affected fields the periodic
noise's fundamental is therefore at most 15,734/15 = 1049 Hz where the chip
would be at up to 1864 Hz — itself far past what a 15.7 kHz sampled model
renders cleanly (the reference aliases it).  It is audible only in the jump's
sweep tail; whether it matters is the listening half of step 1.

### Negative controls

Each mutation below was applied to a scratch copy and must make a check fail.
Re-run 2026-09-26 on the amended model (the failure lists are that run's; the
last three rows are new with the amendment).

| mutation | failed |
|---|---|
| LFSR white tap at bit 2, not bit 1 | E5-E7/E7h synthetic, E20 (the recording has no white noise, which is why the synthetic stream exists) |
| AUDCMD consumed after the voices (a SET lands one tick late) | E5, E6, E7h, E8h, E10, E16, E5-E7/E7h synthetic, E8h synthetic, E20 |
| RESET_PHASE does not zero the phase | E5, E6, E8h, E5-E7/E7h synthetic, E8h synthetic, E20 |
| RESET_PHASE on the LFSR voice does not reseed | E7, E7h, E10, E5-E7/E7h synthetic |
| driver does not re-emit RESET_PHASE on a noise control write | E6, E7, E7h, E8h, E10, E5-E7/E7h synthetic, E8h synthetic |
| INC table off by one at period 85 | stored INC check |
| AMP ladder truncated instead of rounded | E4, E10 |
| LFSR shifts inside the voice walk (the step-1 law) | E7h, E10, E5-E7/E7h synthetic, E20 |
| driver omits the LFSR select SET (select left as power-on garbage) | E6, E8h, E5-E7/E7h synthetic, E8h synthetic, E17, E22 |
| MAX_RECORDS_PER_BOUNDARY 16 (below the first boundary's 26) | E21 (Smurf), and through the slid first boundary E5-E8h, E10, E20 |

The identities E5 and E6 hold for ANY increment by construction, so they do not
catch a wrong INC value (the pitch table and the stored-INC check do) or the
choice between clamping and wrapping the noise INC (E9's row does).

### Listening

`build/wav/reference.wav` and `build/wav/mixer.wav`, 81.7 s each, same format as
the reference's `wav_test` (16-bit mono container, the 8-bit sample x 256,
15734 Hz).  Not yet listened to: that is Brad's half of step 1 (is the LFSR
voice right, is 8-bit AMP enough — note E4's bottom four steps).

### Bare metal

`make m68k` compiles `mixer.cpp` and `psg_driver.cpp` with the 68000 toolchain
at -O2 -Werror; the only undefined symbol is `memset` (no `__udivsi3`,
`__umodsi3` or `__mulsi3`).

## Amendment, 2026-09-26

The model now follows the "Amended 2026-09-26" block of
`../wave-table-design.md` §2; `mixer.h` states each law.  What changed:

1. The LFSR shifts at the END of the tick.  The LFSR voice's carry is latched
   during the walk and the register shifts once after it, so every voice in a
   tick reads one `lfsr[0]` and a carry is heard one tick later than under
   step 1 (where the LFSR voice and later voices heard it the same tick).
2. At most `MAX_RECORDS_PER_BOUNDARY` AUDCMD records (SETs and WAITs) are
   consumed at one boundary; the rest slide to the next.  64 is PROVISIONAL
   until rung (ii) measures the idle gap.
3. `mixer.h` states why two accumulators in one pass are bit-identical to the
   hardware's one accumulator in two passes, and that the model ignores the
   about 7 us left-to-right latch skew.
4. SET window: voices 0..14, 0x78 LFSR voice select, 0x79..0x7F reserved;
   `MAX_VOICES` is 15.  The select is unreset SRAM, so `psg_driver` SETs it
   first in its first emit (17 SETs now, not 16).
5. `mixer.h` says where hardware-private state lives (SRAM >= 0x80, the WAIT
   counter among it) and that the model's 128 bytes are only the SET window.

### Re-derived expectations (from the amended law, before the amended run)

Only the rows the amendment touches.  The new state rows ask the same
question as step 1's; the new "h" rows ask what the output HEARS, which is
where the amendment is visible.

| # | quantity | expectation | why |
|---|---|---|---|
| E6 | noise shift accounting, read after each tick | unchanged: 0 violations | the carry of tick k is still counted in tick k's shift total; only its audibility moves, and the identity is about state after the tick |
| E7 | LFSR state at equal shift counts | unchanged: 0 mismatches | the register passes through the same sequence; only the instant it is read by the voices moves |
| E7h (new) | MIXER output against §2's saturating sum of the four PSG voices, square signs from the phases and the noise sign from the replica LFSR at the shift count reached BEFORE the tick | 0 mismatches, recording and synthetic | the amended law, computed independently of the model's register |
| E8 | rates 0x10/0x20/0x40 | state shift instants still identical (INC == noise_step), rounding-only drift unchanged (0.348 recording / 0.000 synthetic); the HEARD instant is one tick later than the reference's | the reference shifts before it samples, the MIXER after it latches |
| E8h (new) | in epochs where INC == noise_step on every tick since the reseed, the MIXER's heard register at tick t against the reference's register after tick t-1 | 0 mismatches | E8 plus the one-tick delay |
| E10 | sign-agreeing samples, noise sign classified by the HEARD register | still <= 2 LSB | classification now uses what each side actually summed |
| E11 | noise-voice sign disagreements | rise above step 1's 8,197: in exact epochs the heard values differ on every tick where the reference's shift changed bit 0 (2 in 15 shifts for the periodic ring Smurf uses) | E8h |
| E12, E13 | whole-trace max and RMS | E12 still in 62..248; E13 up slightly | E11 adds noise-sign disagreements of 2 x AMP[noise] |
| E17 | first emit | 17 (the select SET, then 16), later <= 13 | change 4 |
| E20 (new) | voice 1 (L) and voice 5 (R) in noise mode around LFSR voice 3 (silent, INC 0xFFFF from phase 0, so a carry on every tick but the first), 400 ticks | L == R == ±AMP of the white replica at max(t-1, 0) shifts on every tick; 399 shifts | one value per tick, heard the tick after the carry |
| E21 (new) | records at one Smurf boundary | the first boundary is the largest: 8 silencing SETs (voices 4..7) + 17 + 1 WAIT = 26; later ones <= 12 + 1; none reaches 64 | the driver's diff emission |
| E21 (new) | limit + 6 SETs, then WAIT 1 | tick 0 takes exactly the limit, tick 1 the 6 and the WAIT | the slide |
| E22 (new) | LFSR voice after the driver's first emit | 3 | change 4 |

### Measured, step-1 model against the amended model (peak 31)

`make test`; all 19 checks pass (18 at peak 60, where E10 is reported).

| # | step-1 model | amended model | verdict |
|---|---|---|---|
| E6 | 0 violations | 0 | held |
| E7 | 0 mismatches, 51 epochs; 0 synthetic | 0, 51 epochs; 0 synthetic | held |
| E7h | (did not exist) | 0 recording; 0 synthetic | held |
| E8 | rounding-only drift 0.348 / 0.000 | 0.348 / 0.000 | held |
| E8h | (did not exist) | 0 of 1,115,370 exact-epoch ticks; 0 of 141,750 synthetic | held |
| E10 | max 1, RMS 0.36 over 319,720 samples | max 1, RMS 0.36 over 319,135 | held |
| E11 | noise sign disagreements 8,197 | 12,970 | held (rose, as derived) |
| E12 | max abs 176 | 188 | held |
| E13 | RMS 26.86 | 27.09 | held |
| E14 | 73.8% differ (948,625) | 73.8% (948,920) | unchanged |
| E15 | envelope mean 0.89, max 18.48; noise blocks mean 1.45, max 10.28 | mean 0.88, max 18.48; noise blocks mean 1.41, max 10.63 | unchanged in substance |
| E17 | first 16, max 12, mean 0.93; 4,578 SETs | first 17, max 12, mean 0.93; 4,579 SETs | held |
| E20 | (did not exist) | 0 wrong of 400, 399 shifts | held |
| E21 | (did not exist) | max 26 records (25 SETs) at the first boundary; 0 boundaries limited; slide test passes | held |
| E22 | (did not exist) | 3 | held |

The numbers are not tuned to anything: E11-E13 moved because the MIXER now
hears noise one tick late, which is the amended law, not a defect.

## Peak 60, 2026-09-26

The smurf app clamps the reference's per-channel peak at 60; §2 lists a
driver peak of 60 as the free alternative to a 10-bit accumulator.  `make
test-peak60` builds the driver's ladder and the reference at 60 (build and
WAVs under `build/peak60/`); the default stays 31.  At 60, E10 and E16's
no-saturation half are reported, not asserted.

Expected, from the ladder arithmetic before the run: AMP 60 48 38 30 24 19 15
12 10 8 6 5 4 3 2 0, so attenuations 11..14 are four distinct steps (no
collapse), worst 0.49 LSB (a8), only a14 more than 0.5 dB off (-1.54 dB);
saturation needs a same-sign sum over 127, which the loud unison passage
(atten 2 4 4 = 38 + 24 + 24) does not reach, so I expected it rare and only
where the noise or a third voice sits near attenuation 0; max abs difference
at the 255 ceiling and RMS about 60/31 x 27.1 = 52.

| quantity | peak 31 | peak 60 |
|---|---|---|
| AMP at attenuations 10..14 | 3 2 2 2 1 (11, 12, 13 collapse to 2: two steps lost) | 6 5 4 3 2 (none lost) |
| worst ladder error | 0.463 LSB (a3); a11 -1.81, a13 +2.19, a14 -1.83 dB | 0.491 LSB (a8); a14 -1.54 dB, all others within 0.50 dB |
| saturating adds (L and R) | 0 | 30,840 on 15,420 ticks (1.2%) |
| reference output at full scale | 0 ticks | 15,470 ticks |
| whole-trace max abs difference | 188 | 255 |
| whole-trace RMS difference | 27.09 | 51.05 |
| sign-agreeing max / RMS difference | 1 / 0.36 | 1 / 0.12 |
| fraction of samples differing | 73.8% | 70.9% |
| output level RMS, ref / mixer | 20.90 / 20.93 | 39.24 / 39.29 |
| envelope difference mean / max | 0.88 / 18.48 | 1.59 / 36.51 |
| SETs over the run | 4,579 | 4,582 |

The saturation expectation was wrong in size: 1.2% of ticks, not rare.  A
scratch tally of the attenuations on saturating ticks puts tone 2 and noise
both at attenuation 0 (60 + 60) with a third voice at 2..5 in almost all of
them (the top combinations: 2 4 0 0, 4 4 0 0, 5 4 0 0, 3 4 0 0).  The
reference clips on almost the same number of ticks (15,470), so this is the
content at that peak, not the MIXER's per-add saturation: the two clip
together, and where every sign agrees they still differ by at most 1 LSB.
Whether the clipping is audible is a listening question
(`build/peak60/wav/{reference,mixer}.wav`).
