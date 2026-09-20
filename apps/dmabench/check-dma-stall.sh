#!/bin/bash
#
# Regression check: the emulator really charges the CPU for ENGINE's video DMA.
#
# Between 2026-08-13 (76c8a9b) and 2026-09-19 service_video() discarded
# VideoState::check_timer()'s return value, so a frame of display list cost the
# emulated 68000 nothing and a 640x480 1bpp console measured 99.9 % of the
# video-off throughput.  The fixed-cap pay-down that followed it was a second,
# quieter version of the same failure: a cap of GRIFFIN_DMA_STALL_STEP cycles
# per CPU step can charge at most STEP/(STEP + mean instruction cycles) of the
# bus, so every list above ~34 % of a line measured the cap instead of its cost.
#
# This runs apps/dmabench for a short field count on the STOCK emulator with a
# CLEAN environment and checks two configurations:
#
#   C1  640x480 1bpp console      expect 76.2 % of the no-DMA throughput
#   C2  pure-VIDCMD MASK text     expect 56.1 %
#
# C1 alone catches "the stall is not charged at all" (it reads ~100 %) but NOT
# the clamp (which happens to land on C1's own answer); C2 catches the clamp (it
# reads 65.6 %).  Both bounds are the analytic column of RESULTS.md +/- 2 pp.
#
# Usage:  apps/dmabench/check-dma-stall.sh [FIELDS]
# Exit:   0 all bounds met, 1 out of bounds, 2 could not run.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
G="$(cd "$HERE/../.." && pwd)"
FIELDS="${1:-40}"

EMULATOR="$G/emulator/build/emulator"
ROM="$G/firmware/rom.bin"
CF="$HERE/dmabench-cf.img"

for f in "$EMULATOR" "$ROM"
do
    if [ ! -x "$f" ] && [ ! -f "$f" ]
    then
        echo "$0: missing $f -- build it first" >&2
        exit 2
    fi
done

if [ ! -f "$CF" ]
then
    echo "$0: $CF missing, building it" >&2
    make -C "$HERE" cf >/dev/null || exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

printf 'DMABENCH %s\r' "$FIELDS" > "$WORK/cmds.txt"

# The environment must be clean: GRIFFIN_NO_DMA_STALL, GRIFFIN_DMA_STALL_LUMP
# and GRIFFIN_DMA_STALL_STEP all change what is being measured here.
unset GRIFFIN_NO_DMA_STALL GRIFFIN_DMA_STALL_LUMP GRIFFIN_DMA_STALL_STEP
export DYLD_LIBRARY_PATH="$G/emulator/build/_deps/sdl3-build"

# Boot plus the six configurations' authoring is the bulk of the run: C2's
# 19,200 MASK records alone are ~200 M cycles, all of it outside the timed
# window.  Six windows of FIELDS fields each on top of that.
CYCLES=$(( 700000000 + FIELDS * 6 * 240000 ))

( cd "$WORK" && "$EMULATOR" --headless --no-throttle --cf "$CF" \
    --console-in "$WORK/cmds.txt" --console-out "$WORK/console.txt" \
    --run-cycles "$CYCLES" "$ROM" ) >"$WORK/stdout.txt" 2>&1

status=0

check()
{
    cfg="$1"; lo="$2"; hi="$3"; what="$4"
    ratio=$(awk -v c="$cfg" '$1 == c { r = $7; sub("%", "", r); print r; exit }' \
            "$WORK/console.txt")
    if [ -z "$ratio" ]
    then
        echo "FAIL $cfg: no result line -- the run did not reach the summary" >&2
        sed -n '/dmabench:/,$p' "$WORK/console.txt" >&2
        status=1
        return
    fi
    if awk -v r="$ratio" -v lo="$lo" -v hi="$hi" 'BEGIN { exit !(r >= lo && r <= hi) }'
    then
        echo "PASS $cfg: $ratio % of no-DMA throughput (expect $lo..$hi, $what)"
    else
        echo "FAIL $cfg: $ratio % of no-DMA throughput, outside $lo..$hi ($what)" >&2
        status=1
    fi
}

check C1 74 78 "640x480 1bpp console, analytic 76.2 %"
check C2 54 58 "pure-VIDCMD MASK text, analytic 56.1 %"

exit $status
