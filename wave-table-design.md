# MIXER (audio Form 2.3): plan

Status: PLAN 2026-09-26.  Target is the time-shared wavetable/PSG voice chip
of `audio-run-voices-proposal.md` §F2.3, chosen to take Smurf's audio off the
CPU (baseline: about 49,000 SYSCLK a field average, 112,000 in the worst
fields, `griffin.log` 2026-09-16) and to make ports of other PSG games cheap.
This file is the plan and the pinned working semantics.  When a fact settles
it moves to `griffin.yml` (registers, records, strobes) or `griffin.log`
(rungs, measurements, rejected alternatives); this file then points there.

## 1. What is being built

* A sixth CPLD, working name MIXER (ATF1508AS), plus one 8K x 8 SRAM from the
  bin (KM6264L-15 / MCM60L64P10) and one more 7200 FIFO pair (AUDCMD).
* The SAMPLES FIFO stays exactly as today (ENGINE `AUDIO_FIFO_W` deposits,
  `{L[15:8], R[7:0]}` two's complement, 0x00 = silence).  MIXER pops it and
  adds it as PCM passthrough; PORTS' audio pop logic retires into MIXER.
* The AUDCMD FIFO is written by ENGINE's existing `ENGINE_SIGNAL_SPARE`
  strobe (mask bit 3, ENGINE pin 9, today routed to a header).  No ENGINE
  change.
* Voice state and wave tables live in the SRAM; one 16-bit phase adder and one
  saturating accumulator per output channel are time-shared across voices
  once per AUDIO_TICK (TIMING, 15,734.375 Hz = 25175000/1600).
* MIXER drives the two DAC latches in front of the existing R-2R ladders.

The first deliverable is the PSG subset (square voices + LFSR noise + PCM
passthrough + pan).  Wavetable lookup is the LAST fit rung and nothing in the
Smurf transition depends on it.  "Wave table" is the eventual capability, not
the scope of steps 1 to 4.

## 2. Pinned working semantics (v0, revise here until they move to griffin.yml)

Tick: one AUDIO_TICK event.  Everything below happens once per tick, in this
order, with the SRAM as the only voice state.

AUDCMD records, one 16-bit word each, consumed at tick boundaries:

```
WAIT { 0, ticks[14:0] }     ticks more ticks elapse before the next record is
                            consumed.  WAIT 0 is a no-op.
SET  { 1, addr[6:0], v[7:0] } write byte v at voice-state address addr.  Takes
                            no time; any number of SETs execute in one tick
                            boundary until a WAIT or an empty FIFO.
```

An empty AUDCMD FIFO changes nothing: the voices keep sounding (a missed
vblank holds the notes, it does not silence them).

Voice state, 8 bytes per voice, 16 voices addressable (addr = voice*8 + i);
the first build implements 8 voices:

```
+0  INC_LO     16-bit phase increment, phase units per tick
+1  INC_HI
+2  AMP        signed 8-bit amplitude
+3  CTL        bit0 PAN_L, bit1 PAN_R, bits 3:2 MODE (0 square, 1 noise,
               2 wavetable, 3 periodic noise (decided during step 1)),
               bit 4 RESET_PHASE (self-clearing)
+4  PHASE_LO   hardware-owned; a SET is honoured (phase reset / sync)
+5  PHASE_HI
+6  WAVE_PAGE  wavetable page in SRAM (MODE 2 only)
+7  reserved
```

Per voice per tick: `phase += inc` (16-bit, wraps); the voice's sample is
* square:    `phase[15] ? +AMP : -AMP`
* noise:     LFSR shifts once on every carry out of the phase add; sample is
             `lfsr[0] ? +AMP : -AMP`.  The LFSR is the SN76489AN's: 15 bits,
             taps 0 and 1 fed back into bit 14 (white); periodic mode is the
             15-bit ring with no feedback.  One LFSR in the chip; which voice
             drives it is a MIXER control byte, v0 says voice 3.
* wavetable: `SRAM[WAVE_PAGE*256 + phase[15:8]]` (rung v; not in v0).

Mix: per output channel an 8-bit two's complement accumulator, cleared each
tick, PCM passthrough added first (the SAMPLES FIFO word if non-empty, else 0
for both channels), then each voice in index order into the channels its PAN
bits enable, saturating on every add.  The two results are latched into the
DACs at the end of the tick.  Amplitude budgeting (so eight voices cannot
saturate) is the driver's business, exactly as `sn76489.h` DEFAULT_PEAK does.

PSG driver (runs on the 68000 in the app; also on the host in the A/B tool):
PSG tone period N -> INC = round(3579545/(32 N) * 65536 / 15734.375);
attenuation a -> AMP from the 2 dB ladder scaled to the chosen peak; noise
control -> the noise voice's INC (rates 0x10/0x20/0x40 or tone 2's period)
and MODE.  Nine PSG bytes a frame become at most about a dozen SETs.

Decided during step 1 (`mixer/mixer.h` and `mixer/psg_driver.h` state each
law in full; these are the points this section was silent on):
* WAIT counting (decided during step 1): a WAIT n consumed at the boundary of
  tick k lets ticks k .. k+n-1 play; the next record is consumed at the
  boundary of tick k+n.  At a boundary the counter first counts down, then
  records are consumed if it is zero.
* Order inside a tick (decided during step 1): AUDCMD records first, so a SET
  takes effect in the same tick's phase add; then the SAMPLES pop; then the
  voices in index order, each sampling its NEW phase (`phase += inc` first)
  and, for noise, the LFSR after any shift made earlier in the same pass
  (including the LFSR voice's own carry).  A noise voice earlier in the order
  than the LFSR voice sees last tick's LFSR.
* Noise feedback (decided during step 1): MODE 1 is white, MODE 3 is periodic;
  the LFSR voice's MODE picks the feedback, and the LFSR shifts only while
  that voice is in MODE 1 or 3.  Any voice in MODE 1 or 3 outputs `lfsr[0]`.
* RESET_PHASE (decided during step 1): zeroes PHASE when the SET executes
  (the first add after it gives phase = inc) and is not stored; on the LFSR
  voice it also reseeds the LFSR to 0x4000.  This is the SN76489's reseed on
  every noise control write, and the driver sends it on every one.  SETs in a
  boundary execute in queue order, so a later PHASE SET wins.
* LFSR voice select (decided during step 1): fixed at voice 3 in v0.  There is
  no address for a MIXER control byte: 16 voices x 8 bytes fill all 128 SET
  addresses.  Step 5 must find one (e.g. give up voice 15, or a reserved +7).
* ±AMP (decided during step 1) is an add or subtract of the signed AMP byte,
  saturating; -(-128) is +128 and clips like any other sum, no 8-bit negate.
* Power-on (decided during step 1): the SRAM is not reset (the model seeds it
  with reproducible garbage, as PaletteUnit does); /RS clears the WAIT counter,
  seeds the LFSR and empties both FIFOs.  Software silences every voice it does
  not own (AMP 0, CTL 0).
* PSG periods 1..7 (decided during step 1): f >= fs, so the true INC is 65536
  or more.  A square voice stores INC mod 65536, which is exact (the same
  alias).  The noise voice stores 0xFFFF: one shift a tick is the most a 16-bit
  INC can carry, below the SN76489's up to 1.8 a sample at rate 3 with tone 2
  at period 4 (Smurf's jump tail, 54 fields).  A real deviation; see
  `mixer/README.md`.
* Driver (decided during step 1): register 0 is period 1024; a SET only for a
  voice byte that changed (mean 0.93 SETs a field over Smurf, max 12; the
  61-tick death frame is 91 AUDCMD words).  A FIFO write when full is lost.

Amended 2026-09-26 after rung (i) and the two-pass mapping review (these
override the step-1 bullets where they conflict; `mixer/mixer.h` and
`cpld/mixer/mixer.v` must both state them):
* Base form is STEREO, ONE accumulator, TWO PASSES over the voices: pass 0
  advances every phase and sums the PAN_L voices, then latches the left DAC;
  pass 1 re-reads PHASE_HI/CTL/AMP and sums the PAN_R voices, then latches
  the right DAC.  The two channels never interact, so the result is bit-
  identical to two accumulators in one pass.  The right latch follows the
  left by about 100 SYSCLK (7 us); the model treats them as simultaneous.
* LFSR shift takes effect at the END of the tick: the carry out of the LFSR
  voice's phase add is latched during the walk and the register shifts once
  after both passes.  Every voice in every pass therefore reads ONE LFSR
  value per tick, and a shift is heard one tick later than in the step-1
  model.  (Replaces "a later voice sees this tick's shift".)  The LFSR itself
  is NOT in rung (ii); it is an optional side rung if cells remain.  Until
  then noise is a wavetable voice (MODE 2) reading a noise table.
* Wavetable read (MODE 2) moves INTO rung (ii): `SRAM[WAVE_PAGE*256 +
  phase[15:8]]` replaces the AMP read for that voice (the table holds the
  signed sample already scaled; volume is a page choice).  A[12:7] are
  therefore live outputs, not constants.
* WAIT is implemented as a 16-bit SRAM field walked by the phase adder
  toward all-ones (loaded as ~n, terminal = carry out), not as a flip-flop
  counter.  The WAIT contract above is unchanged.  Hardware-private state
  lives at SRAM addresses >= 0x80, outside the 7-bit SET window.
* SET address map (provisional, for step 5 to confirm): voices 0..14 at
  voice*8; the block 0x78..0x7F is MIXER control (0x78 LFSR voice select,
  rest reserved).  15 voices maximum.
* SETs per boundary are BOUNDED: the RTL executes AUDCMD records only in the
  idle gap between ticks (about 890 - 268 SYSCLK in the two-pass form, less
  the FIFO read cost), and records beyond that slide to the next boundary.
  The model carries the limit as a constant with a stat and a check; rung
  (ii) measures the real number.  The driver's first-emit (17 SETs, the LFSR select first) must be
  well inside it; measured worst Smurf boundary 26 records.
* SRAM access order is byte-serial with PHASE read first, one 8-bit adder
  used twice, CTL read before AMP; the record layout is unchanged.
* DAC MSB inversion moves into MIXER (decided 2026-09-26): the DAC byte is
  {~acc[7], acc[6:0]}, offset binary for the ladders, so the CD4049 inverters
  are removed from the audio section (rev 1 keeps them: nothing sits between
  the FIFO Q pins and the ladders there).  Samples and AMP stay two's
  complement everywhere else; the 74HC574 latches drive the ladders.
* Open, not decided: a 10-bit accumulator to recover the quiet-end ladder
  (costs cells); or a driver peak of 60 (free, risks saturation with four
  loud voices).  Try the peak-60 A/B before spending cells.

Hand-derived expectations to confirm in step 1 (not to tune to):
* Pitch: the increment quantum is 15734.375/65536 = 0.24 Hz, so the worst
  tone error is at period 1024 (109.2 Hz): 0.12 Hz = 1.9 cents.  Every PSG
  tone therefore lands within 2 cents; the just-noticeable difference is
  around 5 cents, so this should be inaudible.
* Amplitude: the ladder is exact to 1 LSB of AMP.
* Noise: the shift-register sequence is identical to `sn76489.cpp`'s; only
  the shift instants move by phase rounding.
* Per-sample difference against the software SN76489's per-sample renderer
  should be explained entirely by increment rounding (16-bit phase per period
  against the software model's 16.16 phase per half period) and by 8-bit
  amplitude rounding.  Report it; do not adjust the model to shrink it.

## 3. Ladder, with each step's oracle

Cheapest first; a "no" stops the ladder and is logged.

1. **Host model and A/B** (`mixer/`, started 2026-09-26).  `mixer/mixer.{h,cpp}`
   is the shared model of §2 under render.cpp's bare-metal constraints (it
   becomes the emulator's and the suite's model, one copy).  `mixer/psg_driver`
   is the PSG-bytes-to-SETs translator.  `mixer/mixer_ab` plays the recorded
   Smurf PSG stream (`griffin-ports/smurf/work/smurf-trace.io`) through both
   the SN76489 reference (`griffin-ports/smurf/app/audio`, per-sample path) and
   the §2 model and writes both WAVs plus a numeric report (pitch error table
   over all 1024 periods, max and RMS sample deviation, count of differing
   samples).  Oracle: the expectations in §2.  Also answers, by listening: is
   the LFSR noise voice right, is 8-bit AMP enough.
   **GATE PASSED 2026-09-26.**  Pitch worst 1.82 cents, ladder within 0.46
   LSB, invariants hold on every tick, Brad's stereo listening test found no
   glaring difference and possibly preferred the MIXER render (which puts a
   question mark on the SN76489 model, not on §2).  Numbers in
   `mixer/README.md`.  Open from step 1: 8-bit AMP at peak 31 collapses
   attenuations 12 and 13 (a 10-bit accumulator would fix it, at a fit
   cost); noise cannot shift faster than once per tick (SN76489 reaches 1.8
   per sample in the jump tail); no SET address is free for MIXER control
   bytes; the app must queue field k+1's words during field k.
2. **Fit rung (i)** (`cpld/mixer/`, started 2026-09-26, in parallel with 1):
   tick sequencer + SRAM port + 16-bit phase adder + saturating accumulator(s),
   8 square voices, a plain byte write port standing in for the AUDCMD SET
   path, no FIFO ports.  ATF1508AS, `-preassign ignore`, JTAG on, every rung's
   LC/FF/PT/pins in `griffin.log`.  Gate: fits with room for rungs ii to iv
   (target under 100 LC) and pins under 60 of 64 with the FIFO ports counted
   on paper.
   **GATE FAILED 2026-09-26 on logic cells; pins pass only with the shared
   byte bus.**  Ladder in `griffin.log` 2026-09-26: mono 95 LC; stereo with
   two accumulators 126; stereo with one accumulator in two passes 110 (104
   net of six constant address pins that the board should tie instead); 16
   voices 122 in the two-pass form and does not fit with two accumulators.
   Rungs ii to iv need roughly 15 (WAIT) + 10 (FIFO handshakes) + 17 (LFSR)
   more, so §2 as pinned does not fit one ATF1508.  Pins: this rung 56 with
   JTAG; both FIFOs on the shared SRAM byte bus brings the paper total to 45,
   any other arrangement is 60 or more.  SYSCLK per tick 268 of 890 in the
   two-pass form.  Access order is byte-serial with PHASE read first and one
   8-bit adder used twice; CTL must be read before AMP.
   **Open decision (Brad's call) before step 4.**  Candidates, none tried:
   (a) WAIT as a 16-bit SRAM field walked by the phase adder toward all-ones,
   costing accesses not macrocells; (b) noise as a 256-byte table voice in
   place of the LFSR, noting the Smurf recording contains no white noise;
   (c) a second CPLD for AUDCMD and the FIFO ports; (d) mono at 95 LC with
   analog pan.  Tying the constant address pins is free and applies to all.
3. **Datasheets** before any rung is trusted: SRAM read and write cycle
   against the sequencer's three SYSCLK per access; 7200 read timing (tA,
   data hold after /R rises, high-Z) on a shared byte bus; latch setup/hold
   on the DAC latches.  Feedback_timing_discipline: no timing claim without
   the table.
   **DONE 2026-09-26 (griffin.log step 3 entry, every number from a fetched
   table).**  Reads at 3 SYSCLK pass everywhere (smallest margin tOE +64 ns
   on KM6264A-12, +74 on MCM60L64-10); 7200 reads on the shared bus pass
   (tRHZ +48, tDV hold +5); 74HC574 passes; the dead turnaround clock is
   load-bearing (+28 with it, -43 without).  **Address hold after write
   FAILS at 3 clocks** (KM6264A tWR1 5 ns min -> -13 ns worst; MCM60L64 /
   KM6264B need 0 but ATF1508 publishes no tCO minimum): WRITES become 4
   SYSCLK with A/D held through t3, +16 SYSCLK per 8-voice tick (268 ->
   284).  Part is the MCM60L64P10; the KM6264L-15 grade has no published
   table and the A generation's tWR rules it out.  Step 6's SRAM model must
   check tWR with a nonzero tCO skew.
4. **Fit rung (ii): the whole PSG-subset chip** (started 2026-09-26, in
   parallel with step 3).  On the two-pass base: WAIT as an SRAM field walked
   by the phase adder; the AUDCMD FIFO read port on the shared SRAM byte bus
   (empty flag synchronized, records executed in the idle gap, a tick landing
   mid-record held not lost); the SAMPLES pair popped on the same bus, left
   chip at the start of pass 0 and right chip at the start of pass 1, as the
   PCM seed; MODE 2 wavetable read with A[12:7] live; no LFSR.  Then, as
   side rungs only if room remains: + LFSR (MODE 1/3), + 16 voices, F2.2.
   **GATE: every feature Smurf needs fits one ATF1508 with pass-1 placement
   and the real pin count (JTAG included) under 64, without a limitation
   Brad considers substantial (fewer than 8 voices, mono, no PCM, no
   wavetable).  If it fails, the project switches to a Z80 sound processor
   (decided 2026-09-26; the parts delta is a ROM, a decoder, possibly a
   second latch pair and an oscillator, and a probe header).**  Record the
   SETs-per-boundary number and the SYSCLK per tick.
   **RESULT 2026-09-26 (griffin.log "MIXER fit rung (ii)"; re-verified by the
   coordinator: `make mixer` 128/128 LC, 60 FF, 44/64 I/O, pass 1; `make
   mixer-sim` passes, MUTATE=1/2/3 fail).**  PASSES ON THE LETTER: 8 stereo
   voices, PCM seed, MODE 2 wavetable, AUDCMD SET/WAIT with both FIFO ports,
   4-clock writes, DAC MSB inverted in-chip (0 cells).  ZERO spare cells:
   the LFSR does not fit (MODES 1/3 play as square; noise must be a MODE 2
   table, so no white noise), 15 voices do not fit, RESET_PHASE is stored
   and ignored (two PHASE SETs do the same), and the same RTL lands 123-128
   or fails grouping on a single literal.  Measured: 290 SYSCLK/tick (8
   square), 338 (8 tables), idle gap 580/532, SET cost 10, WAIT cost 42,
   SETs-per-boundary bound 53/49; right latch trails left by 77-101 SYSCLK.
   Whether "no LFSR, no headroom" is a substantial limitation is Brad's call
   (open as of this entry).
5. **Pin the encoding**: AUDCMD record formats, the voice-state address map,
   MIXER's own control bytes, the strobe and FIFO assignments, into
   `griffin.yml` (or `super-engine/descriptor.h` until it settles, as VIDCMD
   did), then `make` at root so all consumers regenerate.  PORTS loses its
   audio pop logic and `PORTS_AUDIO_CONTROL/STATUS` move or go: PORTS re-fit,
   firmware and `apps/smurf` edits.  JTAG chain gains MIXER: chain naming and
   `board/label_cpld.py`.
6. **Testbench** `cpld/mixer/mixer_tb.v` with behavioural SRAM and 7200 models
   that check setup/hold, expectations derived from the §2 model before
   simulating, `-DMUTATE=n` negative controls.  The model and the RTL must
   agree tick for tick on the A/B stream.
7. **Shared model into the suite and emulator**: super-engine case m4-audio
   grows a MIXER variant; the emulator replaces the PORTS audio pop model with
   `mixer/mixer.cpp`, `--wav-out` unchanged.
8. **Smurf on MIXER**: the app's audio path becomes psg_driver + a handful of
   SET/WAIT words per frame delivered by the existing display list.  Oracle:
   re-measure the field budget with `GRIFFIN_PROFILE`; the claim is a number
   against the 49,000 / 112,000 baseline.
9. **Prototype**, if practicable: MIXER + SRAM + AUDCMD pair on a breadboard
   fed from the ENGINE spare-strobe header, because steps 1 to 8 never touch
   silicon and the shared byte bus is the one hard timing problem.  If not
   practicable, the Rev 2 audio section is designed as unproven: test points
   and an isolation option.
10. **Rev 2 schematic**: MIXER, SRAM, AUDCMD FIFO pair, DAC latches, the data
    bus fan-out (six more 7200 loads were already a concern; probably buffers).

## 4. Where things live

* `mixer/` — shared model, PSG driver, A/B tool, its README.
* `cpld/mixer/` — RTL, testbench, fit outputs (outputs untracked as elsewhere).
* `griffin.log` — every rung, measurement and rejected alternative, dated.
* `griffin.yml` — the settled contract (step 5 onward).
* `audio-run-voices-proposal.md` — the rationale; superseded here for the plan.
