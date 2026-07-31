#!/bin/bash
#
# Build a raw CF card image (unpartitioned FAT16 "superfloppy") from a staging
# directory, for the emulator's --cf option and for writing to a real card.
#
# Usage:
#     ./mkcf.sh <stage-dir> <output.img> [size]
#
# Example:
#     mkdir -p stage && cp hello/hello.bin exctest/exctest.bin stage/
#     ./mkcf.sh stage basic-cf.img 16m
#     (cd ../emulator && ./build/emulator --cf ../apps/basic-cf.img ../firmware/rom.bin)
#
# macOS only (hdiutil).  -layout NONE makes an unpartitioned volume, which is
# what the firmware's FatFs expects (it mounts sector 0 as the boot sector);
# -format UDTO writes raw sectors to a .cdr, which we rename to the image name.
# Names are uppercased/8.3-mangled by FAT, so stage files with the names the
# guest will type (e.g. FIZZBUZZ.BAS).

set -e -u

if [ $# -lt 2 ] || [ $# -gt 3 ]
then
    echo "usage: $0 <stage-dir> <output.img> [size, default 16m]" >&2
    exit 1
fi

STAGE="$1"
OUT="$2"
SIZE="${3:-16m}"

if [ ! -d "$STAGE" ]
then
    echo "$0: '$STAGE' is not a directory" >&2
    exit 1
fi

# hdiutil appends its own extension, so build to a temporary base name.
BASE="$(dirname "$OUT")/.mkcf-$$"

hdiutil create -size "$SIZE" -fs "MS-DOS FAT16" -volname GRIFFIN -layout NONE \
        -srcfolder "$STAGE" -format UDTO -ov "$BASE" >/dev/null

mv -f "$BASE.cdr" "$OUT"

echo "$OUT: $(ls -l "$OUT" | awk '{print $5}') bytes from $STAGE/"
ls -l "$STAGE"
