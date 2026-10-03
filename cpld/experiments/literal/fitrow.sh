#!/bin/sh
# fitrow.sh rN -- one ladder row from rN.fit / rN_fit.log
r=$1; f=$r.fit
fits=$(grep -q "Design fits successfully" ${r}_fit.log && echo yes || echo NO)
lc=$(grep "^Total Logic cells" $f | awk '{print $5}'); ff=$(grep "^Total Flip-Flop" $f | awk '{print $4}')
nd=$(grep "^Total Nodes+FB" $f | awk '{print $3}'); pt=$(grep "^Total Pts" $f | awk '{print $3}')
p=$(grep -c Fitter_Pass ${r}_fit.log)
tb=$(grep '^[A-H]:' $f | awk '{gsub(/[()]/,"",$(NF-1)); print $(NF-1)" "substr($1,1,1)}' | sort -n | tail -1)
why=$(grep -h -E "Unable to Keep Fanin of [0-9]+ on Block [A-H]|Routing fail|Placement fail|INTERNAL ERROR|pin.*not" ${r}_fit.log $f 2>/dev/null | sort -u | tr '\n' ';' | tr -s ' ')
echo "$r | $fits | $lc | $ff | $nd | $pt | p$p | tightest $tb | $why"
