#!/bin/sh
# harvest.sh rN_pfree -- the fitter's chosen signal pins (from rN_pfree.pin),
# written to pins_rN_pfree.txt as a //PIN block, plus a diff against rev 1.
r=$1
grep '//PIN:' r0.v | grep -v CHIP | sed 's,//PIN: *,,' | awk -F: '{gsub(/ /,"",$1); gsub(/ /,"",$2); print $1, $2}' | sort > /tmp/h_rev1
tr -d '\r' < $r.pin | awk -F: 'NF==2 {gsub(/[ \t]/,"",$1); gsub(/[ \t]/,"",$2); if ($2+0 < 100) print $1, $2}' | grep -v -E '^(TDI|TDO|TMS|TCK) ' | sort > /tmp/h_new
blk() { awk '{p=$2+0; b="?"; if(p<=12||p>=83) b="A"; else if(p<=22) b="B"; else if(p<=31) b="C"; else if(p<=41) b="D"; else if(p<=52) b="E"; else if(p<=61) b="F"; else if(p<=71) b="G"; else b="H"; print $0, b}'; }
{ echo "// harvested by the fitter, $r (free placement) -- NOT FROZEN"; blk < /tmp/h_new | awk '{printf "//PIN: %-17s : %s   (block %s)\n", $1, $2, $3}'; } > pins_$r.txt
moved=$(join /tmp/h_rev1 /tmp/h_new | awk '$2 != $3' | wc -l | tr -d ' ')
dist=$(blk < /tmp/h_new | grep '^RGB_OUT' | awk '{print $3}' | sort | uniq -c | awk '{printf "%s%s ", $2, $1}')
echo "$r: $moved of $(wc -l < /tmp/h_rev1 | tr -d ' ') signal pins moved from rev 1; RGB_OUT per block: $dist"
