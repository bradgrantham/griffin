# Audio proposal: run-record voices on looping FIFOs ("AUDCMD")

Status: PROPOSAL 2026-09-20, written the evening Brad asked for "something
magic for audio" in the spirit of the PIXEL/COMPOSITOR split.  Nothing here is
fitted, simulated, or checked against a datasheet table; every IDT7200 timing
statement below is FROM MEMORY and is listed in section 8 as something to read
out of the datasheet before believing it.  Related `griffin.log` entries:
2026-09-15 "is the 256-entry FIFO the right primitive?", 2026-09-15
"saturating-accumulator fit experiment", 2026-09-16 "measured cost of a
software SN76489", 2026-09-16 "the 256-pair FIFO cannot hold one field".

## 1. The one-paragraph version

Today the audio FIFO holds SAMPLES, so the CPU must compute 15,734 of them per
second per channel; for Smurf's three square waves and a noise voice that is
about 49,000 cycles a field on average (21 % of the machine before DMA
stretch) and about 112,000 in the worst 10 % of fields, even with the
run-length renderer.  Proposal: a voice's FIFO holds **RUN records**, "hold
this level for N ticks" — the audio twin of VIDCMD's `RUN` — at a fast tick
(about 109 kHz), and the 7200's own **retransmit pin** replays a short list of
records for ever.  A square wave is a two-record loop that costs the CPU
nothing while the note holds.  A PCM sample is the same record with a short
count and no loop.  Four such voices are summed in analog.  The software
SN76489 disappears: the game's nine PSG bytes a frame become a handful of
record words delivered by ENGINE.

## 2. The record and the tick

```
RUN   { level[7:0], end_of_loop, count[6:0] }      one 16-bit word
        D[15:8] -> the voice's LEVEL 7200      D[7:0] -> the voice's COUNT 7200
```

* `level`: 8-bit two's complement, 0x00 = silence, exactly today's sample
  format (CD4049 MSB inverter, R-2R ladder).
* `count`: the level is held for `count + 1` ticks.  Tick = SYSCLK / 128 =
  109.375 kHz at 14 MHz (the SN76489 counts at 111.86 kHz, so PSG periods
  convert by one multiply, or a 1024-entry table; pitch is as fine as the real
  chip's).
* `end_of_loop`: after this record's run, replay the voice's records from the
  first one (section 3).  Clear on every record = a one-shot stream (PCM,
  one-shot effects): the voice simply runs dry and holds its last level, so a
  one-shot should end with a level-0 record.
* Runs longer than 128 ticks (1.17 ms; square waves below about 430 Hz) use
  several records of the same level.  A 100 Hz square wave is a 10-record
  loop; the FIFO is 256 deep.
* PCM at the present rate is `count = 6` (15,625 Hz); other rates are other
  counts, per record, so a sample can be pitched by rewriting its count bytes
  and run-length-compressed where it is flat.

## 3. Looping: the 7200 already does it

The IDT7200 has a retransmit input (`/RT`, shared with `/FL`, single-device
mode): pulsing it sets the READ pointer back to the first location while the
write pointer stays, so everything written since the last reset can be read
again.  Valid while no more than 256 words have been written since reset.

So a looping voice is: reset the voice's pair, deposit the loop's records
(the last one flagged `end_of_loop`), and let it run.  The audio chip pops a
record per run; when the flagged record's run ends it pulses `/RT` on that
voice's pair and carries on with record 0.  No write-back path, no buffer on
the data bus, no extra parts: the loop memory is the FIFO.

A note change = voice reset + new records (section 4).  The new note starts at
once and the old half-cycle is cut short.  See section 7 for what that does to
volume envelopes and the alternative that avoids it.

## 4. Getting records to voices with the two strobes ENGINE already has

ENGINE's descriptor mask already has `AUDIO_FIFO_W` and an unused
`nSIGNAL_SPARE` (mask bit 3, header only today).  No ENGINE change:

* **SPARE strobe = control word** to the audio chip (it sits on D[7:0]):
  `{ reset_mask[3:0], select_mask[3:0] }`.  Voices in `reset_mask` get a `/RS`
  pulse (both pointers to zero, loop forgotten).  `select_mask` is latched.
* **AUDIO strobe = a record**, written into every selected voice's pair.  The
  chip gates `nAUDIO_FIFO_W` to the selected voices' `/W` pins.

A note is therefore one descriptor of three words: control (reset + select
voice v), record, record-with-end-flag — 14 + 6 = 20 SYSCLK of bus, no CPU
beyond pointing the descriptor at the words.  A whole frame of Smurf's audio
is at most four of those.  A PCM voice streams with ordinary `count <= 32`
descriptors exactly as today.  Selecting two voices writes the same record to
both (a centred voice, or a unison).

Optional fifth control bit per voice, or a second control word: NOISE mode
(section 6).

## 5. The audio chip and the parts

Per voice: two 7200s (level, count), one 8-bit latch clocked by the rising
edge of that voice's `/R` (the level goes FIFO -> latch -> ladder and never
enters the CPLD), one R-2R ladder.  Four voices summed by resistors into the
existing LM358 stage, voices 0/2 mostly left and 1/3 mostly right (the pan is
two resistors per voice; a mono game wants something like 60/40, not hard
pan).

The CPLD (working name AUDIO; PORTS' audio pop logic retires into it):

* The 7200's outputs are high-Z unless its `/R` is low, so the four COUNT
  FIFOs share ONE 8-bit bus into the chip and the chip pops voices one at a
  time; a tick is 9 microseconds, a pop is tens of nanoseconds.
* Per voice: a 7-bit down-counter, a running/dry state, `/R`, `/W`, `/RS`,
  `/RT` out and `/EF` in.  Guess: 13 macrocells and 5 pins a voice.
* Shared: tick divider (7 FF), control-word latch (8 FF), the pop sequencer.
* Guess for four voices: 75-90 macrocells, about 40 pins -> an ATF1508, not
  the freed 1504 socket.  Two voices might fit the 1504.  Guesses only; a
  ladder settles it (section 9).

Parts for four voices: 8 x 7200 (6 more than today), 4 latches, 4 ladders, one
ATF1508.  For two voices: 4 x 7200, 2 latches, 2 ladders, possibly the 1504.

## 6. Noise

A 15-bit LFSR does not fit a 256-record loop as records (32,767 steps).
Periodic noise does (15 steps), and that is the Smurf cave whoosh.

For white noise, two options, to be settled by listening (section 9 step 1)
and by the fit ladder:

* (a) One voice gets a NOISE mode bit.  In that mode the chip steps an LFSR at
  every record boundary and drives one extra pin with the LFSR's output, which
  inverts the level's sign at that voice's ladder (an XOR gate beside the
  CD4049).  The loop is a single record `{A, n}`: an exact SN76489 noise voice
  for about 17 macrocells and one 74x86.  Its shift rate is the record's
  count, so the SN's "clocked from tone 2" mode is just the driver copying
  tone 2's count.
* (b) No logic: a 256-record pseudo-random loop, and accept its slightly
  metallic, POKEY-like colour.

## 7. What it costs, what it buys, and the honest comparison

Buys:
* Chip music at essentially zero CPU and about 1 % of the bus.  Smurf's audio
  goes from 56-61 % of the available CPU to a few hundred cycles a field
  (translate nine PSG bytes to <= 4 three-word notes by table lookup).
* PCM still works, per voice, mixed in analog with no CPU mixing, at a
  per-record sample rate.  Four voices of effects over music, or stereo
  soundtrack plus two effect voices.
* Duty cycle, multi-level waveforms (a 16-record loop is a 4-bit wavetable
  period: triangle, saw, "SID-ish" shapes), chords by arpeggio loops, PCM
  drum one-shots — one mechanism, all data-driven, in the way VIDCMD is.
* No FIFO-level bookkeeping for tones: a missed vblank leaves the last note
  playing instead of emptying the FIFO (the 2026-09-16 finding).  PCM voices
  still want a level indication; the 7200's `/HF` pin is free for that, and a
  7201 (512 deep) drops into the same socket.

Costs and limits:
* Six more 7200s and an ATF1508 for four voices.
* **Volume changes restart the wave.**  An SN76489 changes attenuation without
  touching phase; here a new level means a reset and a reload, which cuts one
  half-cycle short.  A decaying note updated every field gets a 60 Hz
  irregularity.  I think it is inaudible at these amplitudes and pitches, but
  I do not know; it is the first thing to listen to in the emulator (section
  9, step 1) before any hardware.  The way out if it matters is section 7.1.
* A frequency sweep faster than one step per field has to be approximated
  (Smurf's jump sweeps once per field; fine).
* Nothing here plays FM (YM2151).

Compared with the two obvious alternatives:
* **Buy an SN76489 / AY-3-8910 / YM2149.**  One chip, one chip select, exact
  for its own games, period-correct (the ST and MSX had one).  It does nothing
  for PCM or for any other chip's music.  If the goal were only Smurf, this
  wins on parts.
* **Plain PSG voices in a CPLD** (period register + counter, no FIFOs): about
  21 FF a voice, fewer chips than this proposal, phase-continuous volume — but
  square waves only, and PCM stays a separate mechanism with CPU mixing.

This proposal is the one where tones, waveforms and samples are the same
thing.  Whether that generality is worth six FIFOs is tomorrow-Brad's call.

### 7.1 Alternative B: write-back loops (phase-continuous, in-band note change)

Instead of `/RT`: every popped record flagged "recirculate" is written back at
the FIFO's tail; when new records arrive by DMA the old loop stops being
written back, drains on its last pass, and the new records follow — a note or
volume change lands on a waveform boundary with no reset and no glitch.
Costs: the FIFO data inputs can no longer sit directly on the system bus (the
write-back needs its own drivers: an isolated audio data bus behind two
74HC244s, each voice's record latch with output enable), and the chip must
serialise write-backs against ENGINE deposits it cannot stall.  More parts and
the only hard timing problem in this document.  Only worth it if the
section 7 restart artefact turns out to be audible.

## 8. To verify before believing any of this (datasheet tables, not memory)

IDT7200 (the LP15 parts in hand):
* Retransmit: `/RT` pulse width, recovery time before the next read, the
  requirement that `/R` and `/W` be high during it, and that flags are valid
  after.  Confirm retransmit works in single-device mode with `/XI` grounded
  and that `/FL//RT` is the pin.
* That a write (ENGINE deposit) can never coincide with a `/RT` pulse under
  the section 4 protocol: records are appended to a looping voice only after
  its reset, but the loop may reach its end flag while a multi-record loop is
  still being deposited (a long loop, a short first run).  Probably needs the
  rule "a voice does not start until its end-flagged record has been written"
  — the chip can see the end flag go by on D[7] during the deposit.
* Gating `nAUDIO_FIFO_W` through the CPLD to four `/W` pins: added delay and
  glitch-freedom against the deposit's data setup/hold (`engine.v` strobe
  timing, the same budget the PIXELS/VIDCMD write ports were checked against).
* Latching the level on the rising edge of `/R`: data hold after `/R` high
  (tDV) against the latch's hold time.
* Reset: `/RS` pulse width and recovery before the first write, inside one
  descriptor (control word, then a record two SYSCLK later).  If recovery is
  longer than that, the control word and the records go in separate
  descriptors, or the chip delays nothing and the rule becomes "reset in one
  descriptor, records in the next".

Board: D[15:0] fan-out.  `griffin.yml` "Stream FIFO parts" already counts six
7200 loads directly on the bus; this adds six more.  Probably wants the audio
FIFOs behind one pair of buffers.

## 9. Ladder (cheapest first; stop at the first "no")

1. **Listen, in software, no hardware.**  A host tool that turns the recorded
   Smurf PSG byte stream into run records and plays them through a model of
   this voice (reset-on-change, 109.375 kHz tick, `/RT` loop), written to a
   WAV beside the SN76489 model's WAV.  Answers: is the restart artefact
   audible, is the pitch quantisation fine, does noise option (b) sound
   acceptable.  A day, in `griffin-ports/smurf/app/audio`.
2. Read the section 8 datasheet items.
3. Fit ladder for the AUDIO chip: two voices in an ATF1504; four voices in an
   ATF1508; + NOISE (a); record every rung's LC/FF/PT.
4. Testbench with a behavioural 7200 model including retransmit, expectations
   hand-derived first, mutants that must fail.
5. Emulator voice model + firmware/app API, then the Smurf app's audio path
   becomes a table lookup, and the field budget is re-measured.
6. Rev-2 schematic.

---

# Form 2 (Brad, later the same evening): SAMPLES + AUDCMD + a sixth CPLD

Brad's framing, for symmetry with video: keep a pure SAMPLES FIFO that ENGINE
fills exactly as today; add a synth-COMMANDS FIFO; a sixth chip (an ATF1508,
working name MIXER) pulls commands and produces sound; an app chooses PCM,
synthesis, or both.  Worked through below.  Same caveat as above: nothing
fitted, nothing simulated, all timing from memory.

## F2.1 The symmetry, taken literally

| video | audio |
|---|---|
| PIXELS FIFO: bulk data, one record per line | SAMPLES FIFO: `{L8, R8}` per tick, as today |
| VIDCMD FIFO: `RUN` / `SET` records that occupy slots | AUDCMD FIFO: `WAIT` / `SET` records that occupy ticks |
| COMPOSITOR: passthrough, or its own sources, per slot | MIXER: PCM passthrough PLUS its own voices, per tick |
| palette RAM behind PIXEL | voice/wave RAM behind MIXER (F2.3) |
| nothing staged = HOLD | AUDCMD empty = voices keep sounding |

One change from Brad's sketch, and the reason: the MIXER should drive the DACs
itself and READ the SAMPLES FIFO as its passthrough input, not WRITE synthesis
into the SAMPLES FIFO.  Writing into it means a second driver on FIFO data
inputs that sit directly on the system bus (isolation buffers, arbitration
with ENGINE deposits), and it makes PCM and synthesis either/or.  Reading it
makes sampled effects over chip music the normal case, and COMPOSITOR works
the same way: it consumes PIXEL's output, it does not write PIXELS.
PORTS' audio pop logic retires into MIXER (PORTS gets macrocells back).

AUDCMD records (sketch):

```
WAIT { 0, ticks[14:0] }               time passes; voices and PCM play
SET  { 1, address[6:0], value[7:0] }  write one byte of voice state, takes no time
```

ENGINE's unused SPARE strobe is the AUDCMD FIFO's `/W`: no ENGINE change.  The
tick is locked to the raster (262.5 per field exactly), so `WAIT` counts never
drift against fields; a song is a descriptor list of SET/WAIT words, a game
driver is "this frame's PSG bytes -> a few SETs".  A missed vblank just leaves
the notes sounding (the 2026-09-16 FIFO-runs-dry finding goes away for
synthesis; PCM still wants `/HF`).

## F2.2 Voices inside the chip (a PSG in a 1508): probably does not fit with a digital mix

Three tone voices at 10-bit period + 10-bit counter + flip = 63 FF; noise LFSR
and control about 21; four amplitudes 16-24; WAIT counter 12-15; a serial
saturating accumulator and L/R output registers about 25.  About 140-150 FF
against 128 macrocells, before the adder's product terms.  It comes under 128
only with analog mixing (each voice's gated square wave on its own pins into
resistors, PCM on its existing DACs), which gives linear 4-bit volume, not the
PSG's 2 dB ladder.  Worth one fit rung to confirm; I do not expect it to be
the answer.

## F2.3 Voices in an SRAM, one adder time-shared: the form I would pursue

Brad's bin has 8K x 8 SRAMs (MCM60L64P10, KM6264L-15) that were too slow for
the palette.  They are far faster than audio needs.  A tick is 890 SYSCLK
long; a voice needs about seven byte-wide RAM accesses (increment low/high,
phase low/high read, phase low/high write-back, one wave sample), about 21
SYSCLK at three clocks an access.  So ONE 16-bit phase adder and ONE
saturating accumulator, time-shared, service 8 or 16 voices per tick with
most of the tick to spare, and all voice state — phase, increment, amplitude,
wave select, pan — lives in the RAM, where AUDCMD `SET`s write it.  The same
RAM holds the wave tables (256 bytes each), so a voice is a WAVETABLE voice:
square, pulse, triangle, a sampled single cycle, anything 256 bytes long.
(I believe this is roughly how mid-80s wavetable chips such as Ensoniq's were
organised; from memory, not checked.)

* Volume without a multiplier: a square/noise voice adds or subtracts its
  amplitude byte by the phase MSB (no table at all, phase-continuous volume,
  an exact PSG voice); a wavetable voice gets volume by selecting one of
  several pre-scaled copies of its table (16 volumes x 256 B = 4 KB per
  waveform; 8 KB RAM holds state plus about one such bank, a second RAM chip
  or a 32 KB part holds more).
* Noise: a 15-bit LFSR in the chip stepped on one voice's phase carry (about
  17 macrocells), or a 256-sample noise table (no logic, short-loop colour).
* PCM passthrough: the SAMPLES FIFO's `{L, R}` is simply the first term added
  into the accumulators each tick.  Per-voice pan bits choose L, R or both.
* Output: two external 8-bit latches in front of the existing R-2R ladders.
* Rough size: phase/temporary 16, accumulator 10 x 2 (or one, two passes),
  voice and step counters 8, WAIT 15, address/data staging about 20, LFSR 15
  => about 95-105 FF plus a 16-bit adder and a saturating 10-bit adder in
  product terms.  The 2026-09-15 mixer experiment measured stereo 8-bit
  saturating accumulators with a bus-slave port at 54 LC on a 1504, so an
  ATF1508 is plausible and not certain.  Pins: RAM 13 + 8 + 2, AUDCMD FIFO
  16 + 2, SAMPLES FIFO 16 + 2 (or 8 + 3 if L and R are popped in turn onto a
  shared byte bus), DAC byte + 2 latch clocks, SYSCLK, AUDIO_TICK: about 55-62
  of 64.  Tight; the shared-bus trick (FIFO outputs are high-Z unless read)
  can also put the AUDCMD FIFO bytes and the RAM data on ONE byte bus, which
  brings it to about 40.

Against the run-record voices of sections 1-9: Form 2.3 has phase-continuous
volume (the known weakness above disappears), more voices (8-16 against 4),
wavetables, exact in-band timing, digital mixing with PCM, and fewer FIFOs
(two more 7200s instead of six) — for one SRAM from the bin and a harder CPLD.
Run-record voices keep one advantage: almost no logic.  If 2.3 fits, I think
it is the better machine and the closer twin of the video chain.

## F2.4 Ladder for Form 2

1. Listen first, as in section 9 step 1, but model F2.3: the recorded Smurf
   PSG stream -> SETs -> a host model of time-shared 16-bit phase voices at
   15,734.375 Hz -> WAV beside the SN76489 model's.  (The current software
   model is itself a 16.16 point-sampled accumulator, so a square voice here
   should match it closely; say how closely.)
2. Fit rungs on an ATF1508: (i) sequencer + RAM port + 16-bit phase adder +
   one saturating accumulator, 8 square voices, no FIFO ports; (ii) + AUDCMD
   SET/WAIT; (iii) + PCM passthrough and stereo pan; (iv) + LFSR noise;
   (v) wavetable lookup.  Record LC/FF/PT at every rung; F2.2 as a side rung.
3. Datasheet: the SRAM's read/write cycle against three SYSCLK per access;
   7200 read timing on a shared byte bus.
4. Testbench, model, emulator, API, Smurf — as section 9 steps 4-6.
