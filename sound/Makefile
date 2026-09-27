# Z80 sound processor feasibility test (wave-table-design.md §3; the plan's
# step 3).  See README.md.
#
#   make            rom/build/rom{,-nopcm,-fast}.bin + .syms, harness/build/harness
#   make test       the oracles: (a) NOP ROM cadence at 6 MHz; (b) square voices,
#                   (c) the Smurf trace with the PCM ring active and with
#                   --no-pcm (the NO_PCM ROM), (d) every record type -- b, c, d
#                   at PERIOD T per tick, long enough that no tick overruns, so
#                   the comparison against the model is exact and the T
#                   accounting (which does not depend on the clock) is clean.
#   make test-6mhz  the Smurf trace at the real 6 MHz period (381.33 T), first
#                   STOP ticks: reported, not asserted (rung 1 overruns)
#   make test-8mhz  the same at 8 MHz (508.44 T)
#   make measure    b, c, d again with the FAST (rung 2) ROM for the ladder
#
# The trace is read from the griffin-ports checkout and never written.

PSG_DIR ?= ../../griffin-ports/smurf/app/audio
TRACE   ?= ../../griffin-ports/smurf/work/smurf-trace.io
PERIOD  ?= 16000
STOP    ?= 20000
H        = ./build/harness
ROM      = ../rom/build/rom.bin
ROM_NOPCM = ../rom/build/rom-nopcm.bin
ROM_FAST = ../rom/build/rom-fast.bin

all: rom harness

rom:
	$(MAKE) -C rom

harness:
	$(MAKE) -C harness PSG_DIR=../$(PSG_DIR)

test: all
	@echo "== oracle a: NOP ROM, 6 MHz cadence and T accounting"
	cd harness && $(H) --synthetic nop --mhz 6 --ticks 100000
	@echo "== oracle b: square voices, synthetic SET/WAIT, 10,000 ticks"
	cd harness && $(H) --synthetic square --period $(PERIOD) --ticks 10000 --voices 4 --out build/square
	@echo "== oracle c: Smurf trace, PCM ring active (chunked ahead of each WAIT)"
	cd harness && $(H) --trace ../$(TRACE) --period $(PERIOD) --pcm --out build/smurf-pcm
	@echo "== oracle c: Smurf trace, --no-pcm (NO_PCM ROM)"
	cd harness && $(H) --trace ../$(TRACE) --period $(PERIOD) --no-pcm --rom $(ROM_NOPCM) --out build/smurf-nopcm
	@echo "== oracle d: every record type, uploads, the overrun fallback"
	cd harness && $(H) --synthetic records --period $(PERIOD) --out build/records

test-6mhz: all
	@echo "== Smurf trace at 6 MHz, first $(STOP) ticks, PCM"
	-cd harness && $(H) --trace ../$(TRACE) --mhz 6 --pcm --stop $(STOP) --no-wav
	@echo "== Smurf trace at 6 MHz, first $(STOP) ticks, --no-pcm"
	-cd harness && $(H) --trace ../$(TRACE) --mhz 6 --no-pcm --rom $(ROM_NOPCM) --stop $(STOP) --no-wav

test-8mhz: all
	@echo "== Smurf trace at 8 MHz, first $(STOP) ticks, PCM"
	-cd harness && $(H) --trace ../$(TRACE) --mhz 8 --pcm --stop $(STOP) --no-wav
	@echo "== Smurf trace at 8 MHz, first $(STOP) ticks, --no-pcm"
	-cd harness && $(H) --trace ../$(TRACE) --mhz 8 --no-pcm --rom $(ROM_NOPCM) --stop $(STOP) --no-wav

measure: all
	@for v in 1 2 3 4; do echo "== FAST NO_PCM ROM, $$v square voices"; \
	  (cd harness && $(H) --synthetic square --period $(PERIOD) --ticks 10000 --voices $$v --rom $(ROM_FAST) --no-wav | grep "NMI T\|mismatches"); done
	@echo "== FAST NO_PCM ROM, Smurf trace"
	cd harness && $(H) --trace ../$(TRACE) --period $(PERIOD) --no-pcm --rom $(ROM_FAST) --out build/smurf-fast

clean:
	$(MAKE) -C rom clean
	$(MAKE) -C harness clean

.PHONY: all rom harness test test-6mhz test-8mhz measure clean
