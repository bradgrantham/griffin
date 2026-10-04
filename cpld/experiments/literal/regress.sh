#!/bin/sh
# regress.sh rN [narrow] [keep_run_colour]
#
# Runs the PRODUCTION bench cpld/compositor/compositor_tb.v, unmodified,
# against rung rN (compiled with -DLIT_TB, which only exposes a constant
# cur_colour for the one probe of it).  Every check outside the excluded set
# must PASS.  Excluded, decided before running:
#   - every check of the five cases that are built from RUN_COLOR words
#     (RUN_COLOR*, ONE_PX_SPANS, SUSTAINED_2SLOT, RLE_TRUNCATION,
#     MASKB_STARVE), unless the rung keeps RUN_COLOR (3rd arg "keep");
#   - with "narrow" (10-bit counter), the four checks that compare
#     dut.run_count against a 12-bit literal.
r=$1; narrow=$2; keep=$3
pat='^$'
[ "$keep" = keep ] || pat='RUN_COLOR|ONE_PX_SPANS|SUSTAINED_2SLOT|RLE_TRUNCATION|MASKB_STARVE'
[ "$narrow" = narrow ] && pat="$pat|MASKB_BLANK the count is still terminal|MASKB_RESUME the count froze|MASKB_NRS count already terminal|NRS_RECOVERY count already terminal"
iverilog -g2012 -DLIT_TB -o t_reg_$r.vvp ../../compositor/compositor_tb.v $r.v || exit 1
vvp t_reg_$r.vvp > reg_$r.log 2>&1
total=$(grep -c '^\(PASS\|FAIL\)  ' reg_$r.log)
bad=$(grep '^FAIL  ' reg_$r.log | grep -v -E "$pat" | wc -l | tr -d ' ')
exf=$(grep '^FAIL  ' reg_$r.log | grep -E "$pat" | wc -l | tr -d ' ')
exn=$(grep '^\(PASS\|FAIL\)  ' reg_$r.log | grep -E "$pat" | wc -l | tr -d ' ')
echo "$r: production bench $total checks; $bad unexpected failures; excluded $exn (of which $exf fail)"
grep '^FAIL  ' reg_$r.log | grep -v -E "$pat" | sed 's/^/   UNEXPECTED /'
[ "$bad" = 0 ]
