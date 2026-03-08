# cpld/Makefile — ATF15xx CPLD synthesis via oss-cad-suite + atf15xx_yosys
#
# Required variables (set on command line, in environment, or in .config.mk):
#   OSS_CAD_SUITE — path to extracted oss-cad-suite directory
#   ATF15XX_YOSYS — path to extracted atf15xx_yosys directory
#
# Example:
#   make OSS_CAD_SUITE=~/tools/oss-cad-suite ATF15XX_YOSYS=~/tools/atf15xx_yosys

# Load local tool paths if present (.config.mk is gitignored)
-include .config.mk

OSS_CAD_SUITE ?= $(error Set OSS_CAD_SUITE=/path/to/oss-cad-suite)
ATF15XX_YOSYS ?= $(error Set ATF15XX_YOSYS=/path/to/atf15xx_yosys)
WINEDEBUG="-all"
MVK_CONFIG_LOG_LEVEL=0

export PATH := $(OSS_CAD_SUITE)/bin:$(PATH)

YOSYS  := $(ATF15XX_YOSYS)/run_yosys.sh
FITTER := $(ATF15XX_YOSYS)/run_fitter.sh

.PHONY: all glue clean

all: glue

# ------------------------------------------------------------
# GLUE (ATF1508AS)
# ------------------------------------------------------------

glue: glue/glue.jed

glue/glue.edif: glue/glue.v ../griffin.generated.vh
	cd glue && $(YOSYS) glue

glue/glue.jed: glue/glue.edif
	cd glue && $(FITTER) glue

# ------------------------------------------------------------

clean:
	rm -f glue/glue.edif glue/glue.fit glue/glue.jed glue/glue.vo
