# cpld/experiments/literal/Makefile — COMPOSITOR fit ladder for LITERAL / HOLD
#
# FIT EXPERIMENT ONLY.  Nothing here is a production target.  Synthesis and
# fitter invocation copied verbatim from the production `compositor:` target
# in cpld/Makefile, so R0 reproduces the shipped numbers:
#
#   $(FITTER) compositor -preassign keep -strategy debug = on \
#       -strategy TDI_pullup = on -strategy TMS_pullup = on \
#       -strategy power_reset on -strategy JTAG = on
#
# Every rung rN.v is a copy of compositor.v carrying the production //PIN:
# block unchanged (run_fitter.sh writes rN.pin from it).
#
# Usage:  make r0      (fits r0.v -> r0.fit, r0_fit.log, r0.io)
#         make numbers
#         make sim-r2  (lit_tb.v against a rung; see the sim targets)

-include ../../.config.mk

OSS_CAD_SUITE ?= $(error Set OSS_CAD_SUITE=/path/to/oss-cad-suite)
ATF15XX_YOSYS ?= $(error Set ATF15XX_YOSYS=/path/to/atf15xx_yosys)

export WINEDEBUG=-all
export MVK_CONFIG_LOG_LEVEL=0
export PATH := $(OSS_CAD_SUITE)/bin:$(PATH)

YOSYS  := $(ATF15XX_YOSYS)/run_yosys.sh
FITTER := $(ATF15XX_YOSYS)/run_fitter.sh

# The production COMPOSITOR strategy set, verbatim.
STRAT := -strategy debug = on -strategy TDI_pullup = on -strategy TMS_pullup = on -strategy power_reset on -strategy JTAG = on

# A hung fitter run is bounded (no `timeout` on this host).
ALARM := perl -e 'alarm shift; exec @ARGV' 900

.PRECIOUS: %.edif
.SECONDARY:

%.edif: %.v
	$(YOSYS) $* > $*_yosys.log 2>&1 || (tail -40 $*_yosys.log; false)

%.jed: %.edif
	-$(ALARM) $(FITTER) $* -preassign keep $(STRAT) > $*_fit.log 2>&1
	@grep -E "^Total|ERROR|fits|Warning" $*_fit.log | head -30

RUNGS := r0 r1 r1h r2 r2s r3 r4 r5 r6 r6n r5n r7 r8 r8b r8bh r8bn r9
$(RUNGS): %: %.jed
.PHONY: $(RUNGS) numbers clean

numbers:
	@for f in r*.fit; do echo "== $$f"; grep -E "^Total (Logic|Flip|Nodes|Pts)" $$f | head -4; done

clean:
	rm -f *.edif *.jed *.fit *.io *.tt3 *.tt2 *.pin *.vt *.tmv *.vvp *_fit.log *_yosys.log

# Lever rungs: rNx.v is a byte copy of rN.v fitted with xor_synthesis added
# (the GLUE/VIDEO lever).  Not the production COMPOSITOR strategy set.
%x.v: %.v
	cp $< $@
XRUNGS := r2x r5x r5nx r8x r8bhx r9x
$(XRUNGS): %: %.v
	$(YOSYS) $* > $*_yosys.log 2>&1
	-$(ALARM) $(FITTER) $* -preassign keep -strategy xor_synthesis = on $(STRAT) > $*_fit.log 2>&1
	@grep -E "^Total (Logic|Flip|Nodes|Pts)|fits|not fit|Unable" $*_fit.log $*.fit | sort | uniq
.PHONY: $(XRUNGS)

# ------------------------------------------------------------
# Simulation.  `make sim` runs lit_tb.v (new-feature checks, hand-derived) on
# every rung it applies to, plus the production compositor_tb.v regression
# (regress.sh) and the negative controls.  vvp's exit status is not trusted;
# the verdict is the printed line.
# ------------------------------------------------------------

LIT = @sh -c 'r=$$0; iverilog -g2012 -DLIT_TB "$$@" -o t_lit_$$r.vvp lit_tb.v $$r.v && vvp t_lit_$$r.vvp > lit_$$r.log 2>&1; echo "$$r $$*: `grep RESULT lit_$$r.log`"'
NEG = @sh -c 'r=$$0; iverilog -g2012 -DLIT_TB "$$@" -o t_neg.vvp lit_tb.v $$r.v && if vvp t_neg.vvp | grep -q "^FAIL"; then echo "  good  $$r with $$* fails"; else echo "  BAD   $$r with $$* PASSES"; fi'

sim:
	$(LIT) r2 -DFORM_A
	$(LIT) r3 -DFORM_A -DHOLD
	$(LIT) r4 -DFORM_A -DHOLD
	$(LIT) r5 -DFORM_B
	$(LIT) r6 -DFORM_B -DHOLD
	$(LIT) r6n -DFORM_B -DHOLD
	$(LIT) r8b -DFORM_C
	$(LIT) r8bn -DFORM_C
	$(LIT) r7 -DSTREAM
	$(LIT) r1h -DHOLDONLY
	$(LIT) r2st -DFORM_A -DSTREAM
	$(LIT) r4st -DFORM_A -DHOLD -DSTREAM10
	$(LIT) r5n -DFORM_BN
	$(LIT) r2s -DFORM_A2
	$(LIT) r4s -DFORM_A2 -DHOLD

regress:
	./regress.sh r0 "" keep
	./regress.sh r1
	./regress.sh r1h
	./regress.sh r7 "" keep
	./regress.sh r8b
	./regress.sh r8bn narrow
	./regress.sh r2
	./regress.sh r3
	./regress.sh r4 narrow
	./regress.sh r5
	./regress.sh r6
	./regress.sh r6n narrow
	./regress.sh r2st
	./regress.sh r4st narrow
	./regress.sh r2s
	./regress.sh r4s narrow
	./regress.sh r5n

negative:
	$(NEG) r5 -DFORM_A
	$(NEG) r8b -DFORM_A
	$(NEG) r2 -DFORM_B
	$(NEG) r8b -DFORM_B
	$(NEG) r2 -DFORM_C
	$(NEG) r5 -DFORM_C
	$(NEG) r0 -DSTREAM
	$(NEG) r1 -DHOLDONLY
	$(NEG) r2 -DFORM_A -DHOLD
	$(NEG) r5 -DFORM_B -DHOLD
	@python3 mutate.py > /dev/null
	$(NEG) r8b_m1 -DFORM_C
	$(NEG) r8b_m2 -DFORM_C
	$(NEG) r8b_m3 -DFORM_C
	$(NEG) r4 -DSTREAM10
	$(NEG) r2 -DFORM_A2
	$(NEG) r8b -DFORM_A2
	$(NEG) r2s -DFORM_A
	$(NEG) r5 -DFORM_BN
	$(NEG) r5n -DFORM_B
	$(NEG) r2s -DFORM_A2 -DHOLD

pins:
	@for r in r0 r1 r1h r7 r8b r8bn; do ./pincheck.sh $$r; done

.PHONY: sim regress negative pins

# Part 2: free placement.  `make pfree R=r4st` -> r4st_pfree.v / .fit, harvested pins.
pfree:
	python3 genpins.py $(R) pfree
	$(MAKE) $(R)_pfree.jed
	./fitrow.sh $(R)_pfree
	-./harvest.sh $(R)_pfree
.PHONY: pfree
