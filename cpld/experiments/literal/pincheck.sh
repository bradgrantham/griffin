#!/bin/sh
# pincheck.sh rN -- every frozen //PIN: assignment in rN.v appears unchanged in
# the fitter's output rN.pin (which also lists buried nodes, ignored here).
r=$1
grep '//PIN:' $r.v | grep -v CHIP | sed 's,//PIN: *,,' | awk -F: '{gsub(/ /,"",$1); gsub(/ /,"",$2); print $1":"$2}' | LC_ALL=C sort > /tmp/lit_want_$r
tr -d '\r' < $r.pin | awk -F: 'NF==2 {gsub(/[ \t]/,"",$1); gsub(/[ \t]/,"",$2); if ($2+0 < 100) print $1":"$2}' | grep -v '^\(TDI\|TDO\|TMS\|TCK\):' | LC_ALL=C sort > /tmp/lit_got_$r
if diff /tmp/lit_want_$r /tmp/lit_got_$r > /dev/null; then echo "$r: all $(wc -l < /tmp/lit_want_$r | tr -d ' ') frozen pins unchanged"; else echo "$r: PIN MISMATCH"; diff /tmp/lit_want_$r /tmp/lit_got_$r; fi
