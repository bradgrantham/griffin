# Z80 sound processor: feasibility test

The plan's step 3 (griffin.log 2026-09-26, "Z80 sound processor chosen").
`rom/rom.s` is the ROM synth and record drain in hand-written Z80 assembly
(sdasz80); `harness/` runs it on a z80emu core in lockstep with the C++ model
`mixer/mixer.h`, which is the spec; `audio_records.h` is the record protocol.

    make            build rom/build/rom.bin, rom-nopcm.bin, harness/build/harness
    make test       oracles a-d at 6 MHz
    make test-8mhz  the Smurf trace at 8 MHz
    make measure    the same at a long period, for clean per-tick T histograms

The budget: one AUDIO_TICK is 25175000/1600 = 15,734.375 Hz, so a 6 MHz Z80B
has 6e6 / 15734.375 = **381.33 T per tick** (8 MHz: 508.44 T).

## Expectations, derived by hand before the first run

From the Z80 timing tables (the sdasz80 listing `rom/build/rom.lst` prints
the same T per instruction in brackets), for the ROM as written (rung 1:
general loop, phases in RAM, handlers dispatched by a per-voice address).

### NMI framing

| Piece | T | Instructions |
|---|---|---|
| NMI accept | 11 | push PC, PC = 0x0066 |
| prologue to the tick vector | 89 | EX AF,AF' 4, EXX 4, LD HL,(saved_sp) 16, LD A,H 4, OR L 4, JR NZ 7, LD (saved_sp),SP 20, LD SP,#n 10, LD HL,(vector) 16, JP (HL) 4 |
| epilogue `tick_done` to RETN | 122 | XOR A 4, LD (pcm_cap),A 13, LD SP,(saved_sp) 20, LD HL,#0 10, LD (saved_sp),HL 16, LD A,(overran) 13, OR A 4, JR NZ 7, LD (consec),A 13, EXX 4, EX AF,AF' 4, RETN 14 |
| PCM seed, pair in the ring | 91 | LD HL,(ring_head) 16, LD A,(tail) 13, CP L 4, JR Z 7, LD B,(HL) 7, INC H 4, LD C,(HL) 7, INC L 4, LD A,L 4, LD (head),A 13, JR 12 |
| PCM seed, ring empty | 55 | ... JR Z 12, LD BC,#0x8080 10 |
| NO_PCM seed | 10 | LD BC,#0x8080 |
| LD SP,#window | 10 | |
| `tick_end`: DACs + LFSR check | 54 | LD A,B 4, OUT 11, LD A,C 4, OUT 11, LD A,(carry) 13, RRA 4, JR C 7 |
| WAIT decrement (count > 0) | 51 | LD A,(lo) 13, OR A 4, JR Z 7, DEC A 4, LD (lo),A 13, JP 10 |
| WAIT at 0 | 56 | LD A,(lo) 13, OR A 4, JR Z 12, LD A,(hi) 13, OR A 4, JP Z 10 |
| LFSR shift (when the LFSR voice carried) | 116 | LD HL,(lfsr) 16 ... LD (lfsr),HL 16, XOR A 4, LD (carry),A 13, JR 12 |

Framing total per tick, no PCM, no shift: 11 + 89 + 10 + 10 + 54 + 51 + 122 =
**347 T** before any voice plays.  With the PCM pop: **428 T**.  (As built the
prologue is 99 T and the epilogue 109 T: the re-entry flag became a byte so a
nested NMI leaves HL alone; the sum is the same 347/428 within 10 T.)  That is
already more than the 381 T budget: rung 1 cannot play even one voice at
6 MHz, and the FAST variant below exists to measure how much of that is the
plan's ROM-owned epilogue (vector, stack re-seed, overrun count).

### Per voice (the walk)

Prefix, every voice: POP DE 10, POP HL 10, LD A,L 4, POP HL 10, ADD HL,DE 11,
PUSH HL 11, LD E,A 4, LD A,H 4, RLA 4, POP HL 10, POP HL 10, JP (HL) 4 =
**92 T**.  Then by mode/PAN (the handler, including its JP back):

| Voice | T (positive half) | T (negative half) |
|---|---|---|
| square, PAN L+R | 92 + 55 = 147 | 92 + 60 = 152 |
| square, one PAN | 92 + 36 = 128 | 92 + 41 = 133 |
| PAN none | 92 + 10 = 102 | |
| noise, not the LFSR voice, L+R | 92 + 33 + 55 = 180 | 185 |
| noise, the LFSR voice, white, L+R | 92 + 32 + 33 + 55 = 212 | 217 |
| noise, the LFSR voice, periodic, L+R | 92 + 39 + 33 + 55 = 219 | 224 |
| wavetable L+R (stub 46 + sign 25/42 + add 48) | 92 + 46 + 25 + 48 = 211 | 228 |

The last voice's handler ends at `tick_end` instead of looping (the `_z`
variants), so there is no extra prefix for the voice count.

### Whole ticks

| Configuration | T |
|---|---|
| Smurf: 3 square L+R + 1 LFSR noise white, PCM pair, no shift | 428 + 3x147 + 212 = **1081** |
| the same, NO_PCM | 347 + 441 + 212 = **1000** |
| the same with the LFSR shifting (rate-3 noise: every tick) | + 116 |
| 4 square voices L+R, NO_PCM | 347 + 588 = 935 |
| 1 square voice, NO_PCM | 347 + 147 = 494 |

So the expectation is that **rung 1 fails the gate by about 3x** and that the
6 MHz runs overrun on every tick.  The harness therefore measures T per tick
at a long period (`make measure`, `--period 2000`) where the accounting is
clean; T per instruction does not depend on the clock, and the gate is then
arithmetic on the measured table.

### Drain (main loop), T per record

From the header's FIFO read to the next FIFO read.  Poll loop: LD A,(wait_lo)
13, LD HL,#wait_hi 10, OR (HL) 7, JR NZ 12 -> IN 11, CP 7, JR Z 12: an empty
poll is 30 T, an idle loop around it 72 T.

| Record | T | Path |
|---|---|---|
| WAIT | 11 + 7 + 7 + 4 + 11 + 4 + 4 + 4 + 7 + 4 + 7 + 4 + 7 + 13 + 4 + 13 + 12 + 42 (poll) = ~165 | to `main_loop`, wait check, poll |
| SET INC/PHASE | ~150 | `rec_set` byte store, back to `poll` |
| SET AMP | ~150 + 60 + recompute ~250 = ~460 | magnitude, sign, handler lookup |
| SET CTL | ~150 + 90 + recompute ~250 = ~500 | RESET_PHASE adds ~60 |
| SET 0x78 / 0x79 | ~150 + 14 x recompute = ~3800 | every voice's handler |
| SAMPLES n | ~100 + n x ~200 | per pair: read 40, flip 24, ring_put ~110 |
| LOAD n | ~100 + n x ~110 | per word: read 40, two indexed stores 38, sum 11, loop 21 |
| JUMP | ~100 | |
| VOICE | ~700 | stub copy (LDIR 13 bytes ~280) + recompute |
| RESET | ~1500 | 14 voices silenced + recompute_all |

### The ladder (what was tried in the inner loop)

| Rung | Per square voice (L+R) | Framing | Notes |
|---|---|---|---|
| 0 HL-walk (LD r,(HL) / INC L per byte) | ~200 | | rejected on paper: 11 T per byte read, 11 per byte written |
| 1 SP-walk, general dispatch (this ROM) | 147 | 347 (428 with PCM) | POP/PUSH move 2 bytes in 10/11 T; the handler address lives in the record's +6/+7 |
| 2 FAST framing (`FAST = 1`): no vector, no re-entry check, no stack re-seed | 147 | 11 + 8 + 20 + 10 + 10 + 54 + 51 + 20 + 8 + 14 = 206 (287 with PCM) | drops the plan's ROM-owned epilogue |
| 3 register-resident phases (derived, not built) | 84 | | phases in HL'/IX/IY across ticks; INC via POP DE; AMP via a second POP DE after the add; accumulators in B/C; no mode dispatch, no PHASE SETs honoured without a reload flag |
| 4 term-pair table per voice (derived, rejected) | ~84 | | 512-byte {L,R} table per (AMP, PAN) indexed by phase[15:9]; would need the drain to rebuild 256 entries on every AMP SET |

Rung 3's floor: POP DE 10, ADD HL,DE 11, POP DE 10 (AMP), LD A,H 4, RLA 4,
JR 7, then two saturating adds 2 x 19 = 84 T.  Four such voices are 336 T,
before any framing, PCM or LFSR: the 6 MHz budget of 381 is not reachable for
4 voices in any of these forms; 8 MHz (508) fits rung 3 with no PCM and no
headroom.

## Measured

All runs: `harness/build/harness` on the z80emu core, the model in lockstep,
`--period 16000` (no tick overruns, so the T accounting is clean; T per
instruction does not depend on the clock).  Every DAC pair compared bit for
bit against `Mixer::tick()`; wavetable voices against the ROM's documented
law on top of the model.

### Oracles

| Oracle | Result |
|---|---|
| a. NOP ROM, 6 MHz | 100,000 NMIs, period 381.3307 T carried exactly, handler 61 T every time (hand count 11 + 50), acceptance at most 12 T late (the JR $ main loop), 0 failures |
| b. square voices, synthetic SET/WAIT (INC/AMP/PAN/RESET_PHASE/PHASE_HI changes, WAITs above 255) | 10,000 ticks, **0 mismatches** (rom.bin, rom-nopcm.bin, rom-fast.bin) |
| c. Smurf trace, PCM ring active | 1,285,764 ticks (81.7 s), **0 mismatches**, 0 FIFO overflows, 0 underflows |
| c. Smurf trace, `--no-pcm` (NO_PCM ROM) | 1,285,764 ticks, **0 mismatches** |
| d. every record type: square + white + periodic noise with the LFSR shifting every tick, live PCM with the repeat bit, LOAD + VOICE wavetable, RESET, an uploaded tick, a JUMP refused after a bad LOAD, a JUMP taken, a slow uploaded tick overrunning, fallback after exactly 3 overruns | 1,900 ticks, **0 mismatches**, fallback seen, ROM overrun counter 3 |

### NMI T per tick (accept to RETN)

| Configuration | worst | mean | histogram |
|---|---|---|---|
| Smurf (3 square + 1 LFSR white noise, mono), PCM ring active | **1263** | 1186.5 | 1120-1151 (no shift) and 1248-1263 (shift) |
| Smurf, NO_PCM ROM | **1234** | 1105.7 | 1040-1071 / 1152-1183 |
| 4 square voices (random PAN/sign), rom.bin, ring empty | 1055 | 940.6 | |
| 4 square voices, rom.bin, PCM pairs | 1078 | 976.5 | |
| FAST NO_PCM ROM, 1 / 2 / 3 / 4 square voices | 445 / 607 / 764 / 892 | 364 / 501 / 645 / 778 | +137, +144, +133 per voice |

Hand count vs measured: the Smurf tick was predicted at 1081 (PCM) / 1000
(NO_PCM) plus 116 when the LFSR shifts; measured 1120-1151 / 1040-1071 without
the shift and 1248-1263 / 1152-1183 with it.  The 40-70 T difference is the
prologue/epilogue as built (208 vs the 211 counted) and the psg_driver's
voices being LFSR-select and periodic/white variants (212-224) rather than the
147 assumed for all four.  Per square voice the ladder measures ~138 T for a
random PAN/sign mix against 147 (L+R) / 128 (one channel) / 102 (none).

### Drain, T per record (header FIFO read to the drain's return to its poll loop, NMI time excluded)

| Record | mean | max | Notes |
|---|---|---|---|
| SET INC / PHASE | ~150 | | byte store |
| SET (Smurf mix: INC, AMP, CTL) | 301 | 4268 | AMP/CTL include the handler recompute (~500); the max is LFSR_VOICE_SELECT / VOICE_COUNT recomputing all 14 voices |
| WAIT | 113 | 113 | |
| SAMPLES 32 pairs | 9310 | 10203 | ~290 T per pair: word read, bit-7 flip, ring_put with its cap and full checks |
| LOAD (byte per word) | ~110 per byte + ~250 | 8483 (64 bytes) | |
| JUMP | 243 | 270 | |
| VOICE | 1434 | | 13-byte stub copy + recompute |
| RESET | 6241 | | 14 voices silenced + recompute_all |

Drain capacity per gap at 6 MHz, were the tick to fit: (381 - tick) / 300 T
per SET.  With the rung-3 floor (4 voices ~336 + framing) there is no gap at
all: SETs would only ever run in ticks the synth spends idle.

### The GATE

* 6 MHz, 4 voices + PCM: worst NMI 1263 T + drain: **fails by 3.3x**
  (1263 > 381 before any drain).
* 6 MHz, 4 voices, PCM disabled: worst NMI 1234 T: **fails by 3.2x** (needs
  <= 324 for 15 % headroom).
* 8 MHz (508.44 T): fails by 2.5x either way.
* Run at the real 6 MHz period (`make test-6mhz`, measured on the first 5000
  ticks): the ROM's init itself is swamped (each idle NMI costs 268 of every
  381 T, init takes 5.2 M T), then every tick overruns, the nested NMIs cut
  the running tick short (worst NMI reads 169 T because the inner one returns
  first), 4912 of 5000 pairs mismatch and the FIFO overflows 5078 words; the
  8 MHz run (`make test-8mhz`) is the same picture with the overrun counter
  at 15,970 in 20,000 ticks.  Reported, not asserted.

What fits, from the measured ladder (FAST NO_PCM framing 226 T + ~138 per
square voice): 6 MHz carries ONE square voice (364 mean / 445 worst) with no
PCM and no headroom; 8 MHz carries two (501 / 607: worst over by 100).  The
rung-3 register-resident form (84 T per voice, derived, not built) would give
4 voices at 8 MHz with no PCM and no headroom, or 3 voices at 6 MHz with none.

### Findings for griffin.yml / the datasheet (not edited here)

1. **The sentinel rule covers every word, not just headers.**  The drain
   reads the high byte first and treats 0xFF as empty, and it cannot read the
   low byte to check, because a word may arrive between the two reads and the
   pair would fall out of step.  So no stream word may carry 0xFF in its high
   byte: SET 0x7F reserved (given), a PCM left sample of -1 is sent as -2
   (`audio_records.h sample_word`), LOAD carries one byte per word, addresses
   stay below 0xFF00.  Found by oracle d (a PCM ramp hit -1 and the drain
   stalled for good).
2. **One stream means a WAIT blocks everything behind it.**  PCM deposited on
   a fixed cadence queues behind the field's WAIT 263, overflows the 256-word
   FIFO and starves the ring.  The producer deposits the PCM for a stretch of
   ticks ahead of the WAIT covering it, in chunks (the harness: SAMPLES 32 +
   WAIT 32, as ENGINE line descriptors would).  With that, oracle c is exact.
3. **Records apply one at a time, not per boundary.**  The model consumes a
   boundary's records atomically; the Z80 applies each SET when its drain
   handler stores it, so a tick can fall between the INC_LO and INC_HI SETs of
   one emit (a one-off phase perturbation) or between a CTL's phase-zero and
   handler stores (one tick with the old mode).  The harness commits each
   record to the model at the Z80's store, so the comparison is exact; the
   ROM stores every 16-bit quantity (handler address, phase, WAIT) in one
   instruction so nothing is ever torn.  If boundary atomicity is wanted, the
   tick would have to apply a queue the drain fills (about +24 T per tick plus
   the copies).
4. **The garbage window is honoured**: the ROM's init adopts the unreset SRAM
   window as if SET (|AMP|, sign, CTL mode/PAN), so power-on plays what the
   model plays.  In_tick is a byte with a 1-in-256 power-on hazard in the
   first 30 T after reset (a garbage zero with a garbage vector).
5. **A nested NMI (overrun) clobbers the running tick's A and F** and pushes
   the return address wherever SP is (inside the voice records during the
   walk); there is no free register and no safe stack.  Overruns are counted,
   HL is preserved, and three in a row restore the ROM synth.
