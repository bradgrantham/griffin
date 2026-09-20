# image-tools/clut-experiment/Makefile — the host-only CLUT photo experiment
#
#   make            build the tools
#   make sweep      run the whole sweep (DESIGN.md §5) into out/
#   make report     results.csv -> results.md + index.html
#   make check      the DESIGN.md §6 gates (needs the sweep to have run)
#   make clean      remove the tools; make distclean also removes out/
#
# Nothing here touches the repo outside this directory except to READ
# ../clut_color.h, ../clut_quant.h, ../stb/ and ../../../reference-images/.
#
# CXXFLAGS is ASSIGNED, never defaulted: the environment exports an older
# -std and every tool here is C++23.
CXX      ?= c++
OPT      ?= -O3
CXXFLAGS := -std=c++23 $(OPT)
WARN     := -Wall -Wextra
INCLUDES := -I. -I..

TOOLS := clut_prepare clut_encode clut_decode clut_compare clut_corrupt

.PHONY: all sweep report check clean distclean baseline
.DELETE_ON_ERROR:

all: $(TOOLS)

clut_prepare: clut_prepare.cpp clut_format.h ../clut_color.h
	$(CXX) $(CXXFLAGS) $(WARN) $(INCLUDES) -o $@ $<

clut_encode: clut_encode.cpp clut_format.h ../clut_quant.h ../clut_color.h
	$(CXX) $(CXXFLAGS) $(WARN) $(INCLUDES) -o $@ $<

clut_decode: clut_decode.cpp clut_format.h ../clut_color.h
	$(CXX) $(CXXFLAGS) $(WARN) $(INCLUDES) -o $@ $<

clut_compare: clut_compare.cpp clut_format.h ../clut_color.h
	$(CXX) $(CXXFLAGS) $(WARN) $(INCLUDES) -o $@ $<

clut_corrupt: clut_corrupt.cpp clut_format.h ../clut_color.h
	$(CXX) $(CXXFLAGS) $(WARN) $(INCLUDES) -o $@ $<

# The micro-HAM baseline rung is RETIRED (2026-09-20): micro-HAM went with the
# indexed PIXEL and ../microham_encode no longer exists, so run.sh and check.sh
# skip their mham steps when the binary is absent.  The numbers it produced stay
# in out/results.csv, which is the record this experiment exists to be.
baseline:
	@true

sweep: all
	./run.sh

# Re-renders the page from an existing sweep; it deliberately does NOT run the
# sweep, so that reading the results back is cheap.
report:
	@test -f out/results.csv || { echo "no out/results.csv — run make sweep first"; exit 1; }
	./report.py out/results.csv out

check: all
	./check.sh

clean:
	rm -f $(TOOLS)

distclean: clean
	rm -rf out
