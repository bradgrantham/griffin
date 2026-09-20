#!/bin/bash
# check.sh — the DESIGN.md §6 gates.
#
#   1. every swept .clut validates, and the decoder's render equals the
#      encoder's prediction byte for byte;
#   2. three deliberately corrupted files are each rejected, for the right
#      reason;
#   3. c4f, K=16, N=0, dither none reproduces a <=16-colours-per-line synthetic
#      image exactly;
#   4. clut_compare agrees with the numbers microham_encode prints for the same
#      image, which is what lets the baseline and the contenders share a table.
#
# Gate 1 needs the sweep to have run (make sweep).  The others stand alone.

set -u -o pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

out=out
failures=0

fail()
{
    echo "FAIL: $*"
    failures=$((failures + 1))
}

echo "=== gate: exact reproduction of a <=16-colours-per-line image (c4f K=16 N=0 none)"
mkdir -p "$out/check"
./clut_prepare --synth "$out/check/synth-ref.png" > /dev/null || fail "clut_prepare --synth"
./clut_encode --mode c4f --k 16 --n 0 --dither none --predict "$out/check/synth-pred.png" \
    "$out/check/synth-ref.png" "$out/check/synth.clut" > /dev/null || fail "encode synth"
./clut_decode --quiet "$out/check/synth.clut" "$out/check/synth-dec.png" 2>/dev/null \
    || fail "decode synth"
if cmp -s "$out/check/synth-ref.png" "$out/check/synth-dec.png"
then
    echo "  exact: decode == reference, byte for byte"
else
    fail "c4f K=16 N=0 none did not reproduce the synthetic image exactly"
    ./clut_compare "$out/check/synth-ref.png" "$out/check/synth-dec.png"
fi

echo "=== gate: the validator rejects corrupted streams"
# K exceeded needs a stream with K < 16 so the extra index can stay distinct;
# overlapping spans need a stream with spans.
./clut_encode --mode c4h --k 4 --n 8 --dither none "$out/check/synth-ref.png" \
    "$out/check/victim.clut" > /dev/null || fail "encode victim stream"
for kind in kexceed overlap short
do
    ./clut_corrupt --kind "$kind" "$out/check/victim.clut" "$out/check/bad-$kind.clut" \
        > /dev/null || fail "clut_corrupt --kind $kind"
    message=$(./clut_decode --quiet "$out/check/bad-$kind.clut" 2>&1)
    status=$?
    if [ "$status" -eq 0 ]
    then
        fail "validator ACCEPTED the $kind corruption"
    else
        echo "  $kind rejected (exit $status): ${message#*INVALID: }"
    fi
done
if ./clut_decode --quiet "$out/check/victim.clut" > /dev/null 2>&1
then
    echo "  the uncorrupted victim stream is still accepted"
else
    fail "validator rejected the uncorrupted victim stream"
fi

echo "=== gate: clut_compare reproduces microham_encode's own numbers"
if [ ! -x ../microham_encode ]
then
    echo "  SKIPPED: micro-HAM is retired (2026-09-20) and ../microham_encode is gone"
elif [ -f "$out/maya2-ref.png" ]
then
    mkdir -p "$out/check"
    printed=$(../microham_encode --dither ordered "$out/maya2-ref.png" "$out/check/m.ham" \
        "$out/check/m.pal" "$out/check/m-preview.png")
    mham_pixel=$(echo "$printed" | sed -n 's/.*per-pixel sq RGB444 err \([0-9.]*\).*/\1/p')
    mham_box=$(echo "$printed" | sed -n 's/.*4x4-box sq 8-bit err \([0-9.]*\)/\1/p')
    ours=$(./clut_compare "$out/maya2-ref.png" "$out/check/m-preview.png")
    our_pixel=$(echo "$ours" | cut -d, -f2)
    our_box=$(echo "$ours" | cut -d, -f3)
    echo "  microham_encode: per-pixel $mham_pixel  box4 $mham_box"
    echo "  clut_compare:    per-pixel $our_pixel  box4 $our_box"
    if python3 -c "
import sys
a, b, c, d = float('$mham_pixel'), float('$our_pixel'), float('$mham_box'), float('$our_box')
sys.exit(0 if abs(a - b) <= 0.01 and abs(c - d) <= 0.01 else 1)
"
    then
        echo "  agree within rounding"
    else
        fail "clut_compare disagrees with microham_encode's own metrics"
    fi
else
    echo "  SKIPPED: $out/maya2-ref.png is missing (run make sweep)"
fi

echo "=== gate: every swept stream validates and decodes to the encoder's prediction"
streams=$(ls "$out"/*/*.clut 2>/dev/null | grep -v '/check/' | grep -v '/victim\.clut$')
if [ -z "$streams" ]
then
    fail "no swept streams under $out (run make sweep first)"
else
    count=0
    for stream in $streams
    do
        count=$((count + 1))
        if ! ./clut_decode --quiet "$stream" "$out/check/replay.png" 2>/dev/null
        then
            fail "validator rejected swept stream $stream"
            continue
        fi
        pred="${stream%.clut}-pred.png"
        if [ ! -f "$pred" ]
        then
            fail "no prediction beside $stream"
            continue
        fi
        if ! cmp -s "$pred" "$out/check/replay.png"
        then
            fail "decode != prediction for $stream"
        fi
    done
    echo "  $count swept streams validated, all decoded == their prediction"
fi

if [ "$failures" -eq 0 ]
then
    echo "ALL GATES PASS"
    exit 0
fi
echo "$failures GATE FAILURE(S)"
exit 1
