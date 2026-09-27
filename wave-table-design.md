# Audio: Z80 sound processor (wavetable + PCM) plan

Status: PLAN 2026-09-26, decided the same evening the CPLD form reached its
gate (footnote, §5).  Goal unchanged: take Smurf's audio off the 68000
(baseline about 49,000 SYSCLK a field average, 112,000 worst, griffin.log
2026-09-16) and make PSG-game ports cheap.  Facts that have settled live in
`griffin.yml` (AUDIO peripheral, `AUDIO_REC_*` constants, the "Z80 sound
processor" and "Audio output stage" interfaces entries) and in dated
`griffin.log` entries; this file is the plan and the pointer list.

## 1. What is being built

* A **Z80B at 6 MHz** with a 27C128 program-and-table ROM and one 8K x 8
  SRAM (6264), reading ONE ganged 16-bit 7200 pair (the rev-1 audio FIFO
  footprint; a 7201 drops in) and writing two 74HC574 DAC latches into the
  existing R-2R / LM358 / LM386 stage.  Decode is a 74HC138 and half a
  74HC139; a 4.7 k pull-up pack on the Z80 data bus.  That is the whole
  sound section.
* ENGINE is untouched: every `AUDIO_FIFO_W` deposit is a 16-bit **record**
  (`sound/audio_records.h`): `SET` a voice byte, `WAIT` n ticks, `SAMPLES` n
  inline PCM pairs, `VOICE` (table base and length in the Z80's RAM),
  `LOAD`/`JUMP` (a title uploads its own program and runs it), `RESET`.
* **No status path back to the 68000, by design.**  No /EF readback, no
  reset bit, no flow control.  Deposits are display-list descriptors at known
  lines, scheduled against the drain rates the ROM documents.  The Z80
  needs no /EF input either: the IDT7200 ignores a read while empty and
  leaves Q high-Z (DSC-2679/7 "Read Enable"), so the pulled-up bus reads
  0xFF, which is why SET address 0x7F is reserved.
* **AUDIO_TICK is /NMI.**  The NMI vector is in ROM, so the ROM's tick entry
  runs first under any uploaded program: it re-seeds the stack, calls the
  tick vector in RAM, counts overruns and falls back to the ROM synth.  A
  bad upload degrades to the ROM synth, never to a wedged chip.
* **Debug**: a 12-pin header with /WR, /IORQ, a decoded DEBUG select and
  D0..D7; a Pico 2 or a logic analyzer decodes `OUT (DEBUG),A` as a
  console.  No SIO, no CTC (rejected: the CTC clock made it a computer).
* **The ROM synth** is the wavetable+PCM machine: voices with 16-bit phase
  and increment, signed 8-bit amplitude, L/R pan, square / wavetable /
  LFSR-noise modes, PCM passthrough from the SRAM ring, saturating 8-bit mix
  per channel, MSB inverted on the way to the latches.  Which voices are
  tables and how big is the 68000's choice, so the same machine is "stereo
  PCM" or "many wavetables".

## 2. The laws that carry over from the model

`mixer/mixer.h` (`Mixer::tick()`) remains the executable specification of
one tick and the oracle every Z80 build is compared against bit for bit:
records consumed at the boundary (WAIT n at boundary k plays ticks k..k+n-1),
PCM seeds the two accumulators, voices in index order with PHASE += INC,
sample by MODE, ±AMP into each PAN channel saturating on every add, the LFSR
shifting once at the end of the tick on the LFSR voice's carry, the 8-byte
voice layout at voice*8, control bytes at 0x78.  `mixer/psg_driver` stays
the 68000-side PSG-bytes-to-SETs translator.  The A/B against the SN76489
recording (`mixer/README.md`) is the fidelity gate already passed: pitch
within 1.82 cents, ladder within 0.46 LSB, listening test passed.

What the Z80 changes: WAIT is 14 bits (control records took two bits); the
SET window keeps 128 bytes but voice state, tables and the PCM ring live in
the Z80's own RAM; SETs execute in the idle gap between ticks, so SETs per
boundary are bounded by the measured drain rate; there is no record limit
in hardware, only the FIFO's depth.

## 3. Ladder

1. **Feasibility (`sound/`, started 2026-09-26).**  ROM tick in hand-written
   Z80 assembly, run on the z80emu core with a flat-memory harness that
   deposits the recorded Smurf PSG stream exactly as `mixer_ab` does and
   compares every DAC pair with `Mixer::tick()`.  Oracles: NOP cadence;
   square voices on a synthetic stream (0 mismatches over 10,000 ticks); the
   whole Smurf trace (0 mismatches); every record type on a synthetic
   stream.  **GATE: at 6 MHz (381 T per tick), 4 voices + PCM within the
   tick worst-case; or 4 voices with PCM disabled and at least 15 % headroom.
   8 MHz reported alongside.**  Numbers go to `sound/README.md`, griffin.log
   and the datasheet §5.
2. **Package B**: PORTS drops the rev-1 pop logic (re-fit, pins released);
   the emulator runs the ROM image on a z80emu core in place of the PORTS
   audio model; `apps/smurf` and the firmware deposit records instead of
   samples; the rev-1 `AUDIO_CONTROL`/`AUDIO_STATUS` registers leave
   griffin.yml.
3. **Prototype**: the Z80, ROM, RAM and FIFO pair on a breadboard fed from
   ENGINE's existing audio strobe; the debug header is the console.
4. **Rev 2 schematic**: per the griffin.yml interfaces entries.

## 4. Where things live

* `sound/rom/` the ROM program; `sound/harness/` the z80emu harness;
  `sound/audio_records.h` the encodings; `sound/README.md` the numbers.
* `mixer/` the shared model, the PSG driver, the A/B tool.
* `griffin.yml` the contract; `griffin.log` every measurement and rejected
  alternative, dated; `griffin-audio-datasheet.html` the reference page.

## 5. Footnote: the CPLD MIXER, retired 2026-09-26

Form 2.3 of `audio-run-voices-proposal.md` (time-shared voices in an SRAM
behind an ATF1508) was carried to its gate the same day: host model and A/B
passed (`mixer/`); fit rung (i) 110/128 for the stereo two-pass voice walk;
datasheet step 3 forced 4-clock SRAM writes (address hold) and chose the
MCM60L64P10; fit rung (ii) fitted the whole PSG subset at exactly 128/128
with 44 I/O, pass 1, but with no LFSR, no 15th voice, no RESET_PHASE, and the
same RTL landing 123-128 or failing grouping on one literal.  Judged a
substantial limitation for the porting goal (white noise) and for
maintainability (no headroom).  The ladders, rejected levers and timing
tables are the griffin.log entries "MIXER fit rung (i)", "MIXER step 3" and
"MIXER fit rung (ii)" dated 2026-09-26; the RTL is kept on disk under
`cpld/mixer/`, untracked.
