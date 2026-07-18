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

.PHONY: all glue video engine audio clean

all: glue video engine

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

glue/glue.jed: glue/glue.edif
	cd glue && $(FITTER) glue -preassign keep -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy debug = on -strategy power_reset on -strategy JTAG = on

glue/glue.svf: glue/glue.jed
	cd $(PRJBUREAU) && python3 -m util.fuseconv -d ATF1508AS $(CURDIR)/glue/glue.jed $(CURDIR)/glue/glue.svf

flash-glue: glue/glue.svf
	$(OPENOCD_JTAG) \
	    -c "svf glue/glue.svf -tap tap1.tap" \
	    -c shutdown
	
# ------------------------------------------------------------
# VIDEO (ATF1508AS)
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
	rm -f glue/glue-verify.svf video/video-verify.svf
