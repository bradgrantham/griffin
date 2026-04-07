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
    -c init

.PHONY: all glue video engine clean

all: glue video engine

# ------------------------------------------------------------
# GLUE (ATF1508AS)
# ------------------------------------------------------------

glue: glue/glue.svf

glue/glue.edif: glue/glue.v glue/build_id.vh ../griffin.generated.vh
	@perl -i -pe 's/(BUILD_ID\s*=\s*8\x27d)(\d+)/$$1.((($$2+1)&255))/e' glue/build_id.vh
	@echo "GLUE build ID: $$(perl -ne 'print $$1 if /BUILD_ID\s*=\s*8\x27d(\d+)/' glue/build_id.vh)"
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

video/video.jed: video/video.edif
	cd video && $(FITTER) video -preassign keep -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy debug = on -strategy power_reset on -strategy JTAG = on

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
	rm -f glue/glue-verify.svf video/video-verify.svf
