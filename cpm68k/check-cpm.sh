#!/bin/bash
#
# Regression check: CP/M-68K boots in the emulator, runs the CCP's built-ins
# and the DRI utilities, writes to a drive, and returns to the monitor.
#
# Runs the emulator headless on a fresh card image (CPM.BIN + A.CPM + B.CPM,
# built here) with a scripted console, then looks for the expected output and
# verifies on the host that PIP's copy reached B.CPM through the ROM's FatFs
# and SYS_FSYNC.
#
# Usage:  cpm68k/check-cpm.sh [CYCLES]        (default 600 M SYSCLK)
#         CROSS=m68k-linux-gnu- cpm68k/check-cpm.sh   to build CPM.BIN with a
#         native cross gcc instead of the podman wrappers
# Exit:   0 all checks passed, 1 a check failed, 2 could not run.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
G="$(cd "$HERE/.." && pwd)"
CYCLES="${1:-600000000}"

EMULATOR="$G/emulator/build/emulator"
ROM="$G/firmware/rom.bin"

for f in "$EMULATOR" "$ROM"
do
    if [ ! -f "$f" ]
    then
        echo "$0: missing $f -- build it first" >&2
        exit 2
    fi
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

make -C "$HERE" cf CF="$WORK/cpm-cf.img" ${CROSS:+CROSS="$CROSS"} >"$WORK/make.txt" 2>&1 \
    || { cat "$WORK/make.txt" >&2; exit 2; }

# One CP/M session, typed ahead (the ROM's DUART ring holds 256 bytes).
printf 'cpm\rDIR\rSTAT\rTYPE C.SUB\rPIP B:=A:README.TXT\rDIR B:\rGRIFFIN TIME\rGRIFFIN SR\rGRIFFIN EXIT\r' \
    > "$WORK/cmds.txt"

export DYLD_LIBRARY_PATH="$G/emulator/build/_deps/sdl3-build"
( cd "$WORK" && "$EMULATOR" --headless --no-throttle --cf "$WORK/cpm-cf.img" \
    --console-in "$WORK/cmds.txt" --console-out "$WORK/console.txt" \
    --run-cycles "$CYCLES" "$ROM" ) >"$WORK/stdout.txt" 2>&1

status=0

expect()
{
    if grep -q -- "$1" "$WORK/console.txt"
    then
        echo "PASS $2"
    else
        echo "FAIL $2: '$1' not in the console output" >&2
        status=1
    fi
}

expect "CP/M-68K for Griffin"        "BIOS banner (CPM.BIN started)"
expect "A>"                          "CCP prompt"
expect "PIP     .68K"                "DIR lists the distribution (BDOS directory read)"
expect "FREE SPACE"                  "STAT runs (a relocated DRI program)"
expect "c068 "                      "TYPE reads a file (C.SUB)"
expect "B: README  .TXT"             "DIR B: after PIP (BDOS write path)"
expect "Epoch seconds:"              "GRIFFIN TIME (direct BIOS call)"
expect "SR/CCR as read from user mode: " "GRIFFIN SR (68010: privilege-violation patch)"
expect "Leaving CP/M"                "GRIFFIN EXIT"
expect "loader: app exited"          "back in the monitor"
# (anything typed ahead of "GRIFFIN EXIT" is swallowed by the BDOS's own
# type-ahead buffer while it prints, as on any CP/M, so nothing after it is
# asserted; the keyboard path and the monitor's line mode after an exit are
# covered by hand with --ps2-in, see README.)

# PIP's copy must be on the card: pull B.CPM out of the FAT image and read it.
if mcopy -n -i "$WORK/cpm-cf.img" ::/B.CPM "$WORK/B.CPM" 2>/dev/null \
   && python3 "$HERE/tools/cpmdisk.py" get "$WORK/B.CPM" README.TXT "$WORK/readme.txt" 2>/dev/null \
   && cmp -s -n "$(stat -c %s "$HERE/dist/README.TXT")" "$WORK/readme.txt" "$HERE/dist/README.TXT"
then
    echo "PASS B.CPM holds README.TXT byte for byte (write path + FSYNC)"
else
    echo "FAIL B.CPM does not hold the copied README.TXT" >&2
    status=1
fi

if [ $status -ne 0 ]
then
    echo "--- console output ---" >&2
    cat "$WORK/console.txt" >&2
fi

exit $status
