# cpld/Makefile — ATF15xx CPLD synthesis via oss-cad-suite + atf15xx_yosys
#
# Required variables (set on command line, in environment, or in .config.mk):
#   OSS_CAD_SUITE — path to extracted oss-cad-suite directory
#   ATF15XX_YOSYS — path to extracted atf15xx_yosys directory
#
# Example:
#   make OSS_CAD_SUITE=~/tools/oss-cad-suite ATF15XX_YOSYS=~/tools/atf15xx_yosys

# Load local tool paths if present (.config.mk is gitignored)
# E.g. OSS_CAD_SUITE, ATF15XX_YOSYS, PRJBUREAU
-include .config.mk

# Release installed from https://github.com/YosysHQ/yosys
OSS_CAD_SUITE ?= $(error Set OSS_CAD_SUITE=/path/to/oss-cad-suite)
# Cloned from https://github.com/hoglet67/atf15xx_yosys
ATF15XX_YOSYS ?= $(error Set ATF15XX_YOSYS=/path/to/atf15xx_yosys)
# Cloned from https://github.com/whitequark/prjbureau.git
PRJBUREAU ?= $(error Set PRJBUREAU=/path/to/prjbureau)

export WINEDEBUG=-all
export MVK_CONFIG_LOG_LEVEL=0

export PATH := $(OSS_CAD_SUITE)/bin:$(PATH)

YOSYS  := $(ATF15XX_YOSYS)/run_yosys.sh
FITTER := $(ATF15XX_YOSYS)/run_fitter.sh

OPENOCD_JTAG = sudo openocd -f interface/ftdi/um232h.cfg \
    -c "adapter speed 500" \
    -c "transport select jtag" \
    -c "jtag newtap tap0 tap -irlen 10 -expected-id 0x0150803f" \
    -c "jtag newtap tap1 tap -irlen 10 -expected-id 0x0150803f" \
    -c "adapter speed 2000" \
    -c init

.PHONY: all glue video engine audio ports compositor compositor-sim \
        pixel pixel-sim pixel-mutants timing-sim clut-pair-sim clean

# The default set is the Rev 2 super-engine video path: display-list ENGINE,
# PIXEL (with TIMING folded in) + COMPOSITOR.  VIDEO is the Rev 1 in-band-header design,
# retired from `all` when ENGINE was promoted to the display-list model the
# two can't interoperate with; `make video` still builds it for the Rev 1
# board.
all: glue engine ports compositor pixel

# Single source of truth: regenerate ../griffin.generated.vh (a prereq of the
# .edif targets below) by delegating to the root, keyed to griffin.yml/codegen.py.
../griffin.generated.%: ../griffin.yml ../codegen.py
	@$(MAKE) --no-print-directory -C .. codegen

# ------------------------------------------------------------
# GLUE (ATF1508AS)
# ------------------------------------------------------------

glue: glue/glue.svf

glue/glue.edif: glue/glue.v  ../griffin.generated.vh
	cd glue && $(YOSYS) glue

# xor_synthesis is load-bearing for the Rev 2 GLUE, the same way it is for
# VIDEO: with ~ROM_WE, the PORTS select/IRQ and the direct-bus strobes added,
# the design lands at 63/64 I/O and will not route without the macrocell XOR
# gates.  The fitter does not reach for xor_synthesis on its own here — the
# rev-2 failures escalated to Foldback_logic and left Xor_synthesis = OFF.
# -debug is dropped for the same reason as VIDEO (it has crashed the fitter
# when combined with xor_synthesis); nothing consumes its .vt/.tmv output.
glue/glue.jed: glue/glue.edif
	cd glue && $(FITTER) glue -preassign keep -strategy xor_synthesis = on -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy power_reset on -strategy JTAG = on

glue/glue.svf: glue/glue.jed
	cd $(PRJBUREAU) && python3 -m util.fuseconv -d ATF1508AS $(CURDIR)/glue/glue.jed $(CURDIR)/glue/glue.svf

flash-glue: glue/glue.svf
	$(OPENOCD_JTAG) \
	    -c "svf glue/glue.svf -tap tap1.tap" \
	    -c shutdown
	
# ------------------------------------------------------------
# VIDEO (ATF1508AS) — Rev 1 legacy, not in `all` (see note at `all:`)
# ------------------------------------------------------------

video: video/video.svf

video/video.edif: video/video.v ../griffin.generated.vh
	cd video && $(YOSYS) video

# xor_synthesis is load-bearing for VIDEO: with the micro-HAM decoder the
# design only fits (124/128 logic cells) when the fitter may use the
# macrocell XOR gates; without it every pass fails grouping/routing.
# -debug is dropped here — combined with xor_synthesis it has crashed the
# fitter ("INTERNAL ERROR") while writing the debug simulation files.
video/video.jed: video/video.edif
	cd video && $(FITTER) video -preassign keep -strategy xor_synthesis = on -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy power_reset on -strategy JTAG = on

video/video.svf: video/video.jed
	cd $(PRJBUREAU) && python3 -m util.fuseconv -d ATF1508AS $(CURDIR)/video/video.jed $(CURDIR)/video/video.svf

# ------------------------------------------------------------
# ENGINE (ATF1508AS)
# ------------------------------------------------------------

engine: engine/engine.svf

engine/engine.edif: engine/engine.v ../griffin.generated.vh
	cd engine && $(YOSYS) engine

engine/engine.jed: engine/engine.edif
	cd engine && $(FITTER) engine -preassign keep -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy debug = on -strategy power_reset on -strategy JTAG = on

engine/engine.svf: engine/engine.jed
	cd $(PRJBUREAU) && python3 -m util.fuseconv -d ATF1508AS $(CURDIR)/engine/engine.jed $(CURDIR)/engine/engine.svf

# No flash-engine target yet — ENGINE is not in the JTAG chain on Rev 1

# ------------------------------------------------------------
# AUDIO (ATF1504AS) — FIT EXPERIMENT, not in `all`
#
# Stereo audio peripheral fit test on the smaller 64-macrocell ATF1504.
# Self-contained (no griffin.generated.vh), no //PIN: lines, and fit with
# -preassign ignore so the fitter places pins freely to maximize layout.
# Run on demand: `make audio`, then read audio/audio.fit utilization.
# ------------------------------------------------------------

audio: audio/audio.svf

audio/audio.edif: audio/audio.v
	cd audio && $(YOSYS) audio

audio/audio.jed: audio/audio.edif
	cd audio && $(FITTER) -d ATF1504AS -p PLCC44 audio -preassign ignore -strategy JTAG = on

audio/audio.svf: audio/audio.jed
	cd $(PRJBUREAU) && python3 -m util.fuseconv -d ATF1504AS $(CURDIR)/audio/audio.jed $(CURDIR)/audio/audio.svf

# ------------------------------------------------------------
# PORTS (ATF1508AS) — Rev 2, built by `all` alongside GLUE/VIDEO/ENGINE
#
# Human-interface peripheral CPLD: the PS/2 mouse frame engine, both joystick
# ports, both paddle counters, and the line-rate audio FIFO pop.  The fit
# ladder that chose this feature set (ten `ifdef` wrapper configurations) is
# retired; see pcbv2-ports-design.md for the measurements.
#
# -preassign keep like the other production targets: the fitter honours the
# //PIN: lines in ports.v once the Rev 2 pinout is frozen, and places freely
# until then (run_fitter.sh writes an empty .pin when there are none).
# ------------------------------------------------------------

ports: ports/ports.svf

ports/ports.edif: ports/ports.v ../griffin.generated.vh
	cd ports && $(YOSYS) ports

# No xor_synthesis here: measured identical with and without it (365 PT either
# way on the free fit; 364 as frozen), so PORTS stays on the plain strategy
# set.  -debug is dropped only for symmetry with GLUE/VIDEO; nothing consumes
# its output.
ports/ports.jed: ports/ports.edif
	cd ports && $(FITTER) ports -preassign keep -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy power_reset on -strategy JTAG = on

ports/ports.svf: ports/ports.jed
	cd $(PRJBUREAU) && python3 -m util.fuseconv -d ATF1508AS $(CURDIR)/ports/ports.jed $(CURDIR)/ports/ports.svf

# ------------------------------------------------------------
# COMPOSITOR (ATF1508AS)
#
# VIDCMD display-list player for the super-engine video path.  There is no
# Pinout FROZEN 2026-08-26 in the //PIN: block at the bottom of compositor.v;
# -preassign keep re-verified identical to the harvest fit.  A netlist change
# layout.  Self-contained — no griffin.generated.vh — until the VIDCMD
# encoding migrates into griffin.yml.
#
# `make compositor-sim` runs the iverilog testbench, which is the real
# specification of the timing semantics; it exits nonzero if any check fails.
# ------------------------------------------------------------

compositor: compositor/compositor.jed

compositor/compositor.edif: compositor/compositor.v
	cd compositor && $(YOSYS) compositor

compositor/compositor.jed: compositor/compositor.edif
	cd compositor && $(FITTER) compositor -preassign keep -strategy debug = on -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy power_reset on -strategy JTAG = on

compositor-sim: compositor/compositor_tb.v compositor/compositor.v
	cd compositor && iverilog -g2012 -o compositor_tb.vvp compositor_tb.v compositor.v && vvp compositor_tb.vvp

# ------------------------------------------------------------
# PIXEL (ATF1508AS PLCC84) — the indexed decoder WITH TIMING FOLDED IN
#
# One chip: pixel.v `include's timing.v and instantiates Timing, so the three
# raster strobes are internal nets and TIMING's other outputs are PIXEL pins.
# There is no separate `timing` fit target any more — the standalone
# ATF1504AS PLCC44 TIMING chip is the pre-merge design and is reachable at the
# rev1-video tag.  The 2026-08-13 measurement that forced the split
# (pixel_combined.v, ~153 logic cells) was of the MICRO-HAM, 12-bit-colour
# PIXEL; index-only is a different-sized chip and the merge fits with room to
# spare (cpld/pixel/LANDING-NOTES.md).
#
# timing.v is NEVER compiled on its own: it has no //PIN block and no fit
# target, and anything that compiles pixel.v already has it.
#
# Three testbenches, all of which must pass before a semantic change is done
# (CLAUDE.md, video-chain discipline):
#   pixel-sim      the decoder: modes, fetch cadence, skip law, words/line
#   timing-sim     the raster laws, run against the MERGED top level
#   clut-pair-sim  COMPOSITOR + PIXEL + a model of the three palette RAMs
# ------------------------------------------------------------

pixel: pixel/pixel.jed

pixel/pixel.edif: pixel/pixel.v pixel/timing.v
	cd pixel && $(YOSYS) pixel

pixel/pixel.jed: pixel/pixel.edif
	cd pixel && $(FITTER) pixel -preassign keep -strategy debug = on -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy power_reset on -strategy JTAG = on

# `make pixel-sim` runs the iverilog testbench, which is the real
# specification of the decoder's per-mode index semantics, the fetch cadence,
# the skip law and the words-per-line rule; it exits nonzero if any check
# fails.  It then recompiles the bench against each single-mistake mutant and
# requires every one of those runs to FAIL — a bench that cannot fail is not a
# spec.  The mutants are generated into pixel/mutants/ and are not checked in.
pixel-sim: pixel/pixel_tb.v pixel/pixel.v pixel/timing.v
	cd pixel && iverilog -g2012 -o pixel_tb.vvp pixel_tb.v pixel.v && vvp pixel_tb.vvp

pixel-mutants: pixel/pixel_tb.v pixel/pixel.v pixel/mutate.py
	cd pixel && python3 mutate.py
	@for m in `cd pixel/mutants && ls *.v | sed 's/\.v$$//'`; do \
	    (cd pixel && iverilog -g2012 -o mutants/$$m.vvp pixel_tb.v mutants/$$m.v) || exit 1; \
	    if (cd pixel && vvp mutants/$$m.vvp 2>&1 | grep -q "RESULT: FAIL"); then \
	        echo "  good  $$m fails the bench, as a negative control must"; \
	    else \
	        echo "  BAD   $$m PASSES — the bench cannot see this mistake"; exit 1; \
	    fi; \
	done

# `make timing-sim` runs the iverilog testbench, which is the executable form
# of the griffin.yml "TIMING output contracts" entry, against the MERGED top
# level (the strobes are probed as internal nets).  It exits nonzero if any
# check fails.  It then recompiles the bench with each -DMUTATE=n negative
# control (one observed signal perturbed by one clock or one bit) and requires
# every one of those runs to FAIL.
TIMING_MUTATIONS := 1 2 3 4 5 6 7 8 9 10
timing-sim: pixel/timing_tb.v pixel/pixel.v pixel/timing.v
	cd pixel && iverilog -g2012 -o timing_tb.vvp timing_tb.v pixel.v && vvp timing_tb.vvp
	@for n in $(TIMING_MUTATIONS); do \
	    (cd pixel && iverilog -g2012 -DMUTATE=$$n -o timing_tb_mut.vvp timing_tb.v pixel.v \
	        && vvp timing_tb_mut.vvp > timing_tb_mut$$n.log 2>&1); \
	    if [ $$? -eq 0 ]; then echo "NEGATIVE CONTROL MUTATE=$$n DID NOT FAIL"; exit 1; \
	    else echo "negative control MUTATE=$$n fails as required"; fi; \
	done
	@rm -f pixel/timing_tb_mut.vvp pixel/timing_tb_mut*.log

# `make clut-pair-sim` runs the CHIP-PAIR bench: production compositor.v
# UNMODIFIED, the merged PIXEL, and a behavioural model of the three 16x4
# separate-I/O palette RAMs with COMPOSITOR's set_pix_value on their data
# inputs.  This is what pins the palette-write window rule that the
# display-list author has to obey, and it is the only bench that can see an
# unloaded palette entry reaching the DAC.
clut-pair-sim: pixel/clut_pair_tb.v pixel/pixel.v pixel/timing.v compositor/compositor.v
	cd pixel && iverilog -g2012 -o clut_pair_tb.vvp clut_pair_tb.v pixel.v ../compositor/compositor.v && vvp clut_pair_tb.vvp

# ------------------------------------------------------------

flash-video: video/video.svf
	$(OPENOCD_JTAG) \
	    -c "svf video/video.svf -tap tap0.tap" \
	    -c shutdown

# ------------------------------------------------------------

flash-engine: engine/engine.svf
	sudo openocd -f interface/ftdi/um232h.cfg \
	    -c "adapter speed 500" \
	    -c "transport select jtag" \
	    -c "jtag newtap tap0 tap -irlen 10 -expected-id 0x0150803f" \
	    -c init \
	    -c "svf engine/engine.svf -tap tap0.tap" \
	    -c shutdown

# ------------------------------------------------------------
# TODO: OPENOCD_JTAG above declares a two-TAP chain (VIDEO=tap0, GLUE=tap1),
# which is the Rev 1 board.  Rev 2 has four CPLDs in the chain, but the
# physical TDI->TDO order is fixed when the board is routed, so the tapN
# index for PORTS is not knowable yet.  Until then this programs a lone
# PORTS chip on the bench as a single-TAP chain, like flash-engine.

flash-ports: ports/ports.svf
	sudo openocd -f interface/ftdi/um232h.cfg \
	    -c "adapter speed 500" \
	    -c "transport select jtag" \
	    -c "jtag newtap tap0 tap -irlen 10 -expected-id 0x0150803f" \
	    -c init \
	    -c "svf ports/ports.svf -tap tap0.tap" \
	    -c shutdown

# ------------------------------------------------------------
# Verify-only SVF extraction
#
# The programming SVF already contains a full verify pass at the
# end.  Extract just the header + ISC enable + verify words +
# ISC disable so we can read back fuses without erasing/reprogramming.
# ------------------------------------------------------------

define extract_verify_svf
	@echo "Extracting verify-only SVF from $<"
	@sed -n '1,/^STATE IDLE/p' $< > $@
	@sed -n '/^\/\/ ISC verify word/,$$p' $< >> $@
endef

glue/glue-verify.svf: glue/glue.svf
	$(extract_verify_svf)

video/video-verify.svf: video/video.svf
	$(extract_verify_svf)

read-glue: glue/glue-verify.svf
	$(OPENOCD_JTAG) \
	    -c "svf glue/glue-verify.svf -tap tap1.tap" \
	    -c shutdown

read-video: video/video-verify.svf
	$(OPENOCD_JTAG) \
	    -c "svf video/video-verify.svf -tap tap0.tap" \
	    -c shutdown

# ------------------------------------------------------------

clean:
	rm -f glue/glue.edif glue/glue.fit glue/glue.jed glue/glue.vo
	rm -f video/video.edif video/video.fit video/video.jed video/video.vo
	rm -f engine/engine.edif engine/engine.fit engine/engine.jed engine/engine.vo
	rm -f audio/audio.edif audio/audio.fit audio/audio.jed audio/audio.vo
	rm -f ports/ports.edif ports/ports.fit ports/ports.jed ports/ports.vo ports/ports.svf
	rm -f compositor/compositor.edif compositor/compositor.fit compositor/compositor.jed compositor/compositor_tb.vvp
	rm -f pixel/pixel.edif pixel/pixel.fit pixel/pixel.jed pixel/pixel.vo pixel/pixel_tb.vvp
	rm -f pixel/timing_tb.vvp pixel/clut_pair_tb.vvp
	rm -rf pixel/mutants
	rm -f glue/glue-verify.svf video/video-verify.svf
