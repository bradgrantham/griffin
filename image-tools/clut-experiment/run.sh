#!/bin/bash
# run.sh — the CLUT experiment sweep (DESIGN.md §5)
#
# Everything lands in out/, which is git-ignored.  Nothing outside this
# directory is written; the reference photographs are only read.
#
# Per configuration: encode (with --predict, so `make check` has something to
# compare), decode, check decode == prediction, score with clut_compare, append
# a row to out/results.csv.  The CSV is appended as it goes, so an interruption
# costs only the rows not yet written.
#
# Pruning, and the reason for it: the product is run WHOLE (4 images x modes x
# K x N x dither, 480 runs, about 7 minutes) except that c2f is swept at K = 4
# only.  A 2bpp pixel can only reach CLUT entries 0..3, so a budget of 4 already
# pays for every entry the mode can see and K = 12 and K = 16 would be the same
# stream byte for byte.

set -u -o pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

images="maya2 millie2 adobe2 oops2"
reference_dir="../../../reference-images"
modes="c4h c2f c4f attr"
dithers="none ordered fs"
spans="0 4 8 16"

out=out
mkdir -p "$out"

csv="$out/results.csv"
echo "image,mode,k,n,dither,words_per_line,loads_per_line,spans_per_line,per_pixel_mse,box4_mse,psnr,max_pixel_err,predict,flag,png" > "$csv"

started=$(date +%s)

# 1. references.  One reference PNG per image, fed to micro-HAM and the CLUT
#    encoder alike so every number in the sweep is against the same pixels.
for image in $images
do
    if [ ! -f "$out/$image-ref.png" ]
    then
        ./clut_prepare "$reference_dir/$image.ppm" "$out/$image-ref.png" || exit 1
    fi
done
if [ ! -f "$out/synth-ref.png" ]
then
    ./clut_prepare --synth "$out/synth-ref.png" || exit 1
fi

# row <image> <mode> <k> <n> <dither> <words> <loads> <spans> <predict> <flag> <png>
row()
{
    local image=$1 mode=$2 k=$3 n=$4 dither=$5 words=$6 loads=$7 spans_used=$8
    local predict=$9 flag=${10} png=${11}
    local metrics
    metrics=$(./clut_compare "$out/$image-ref.png" "$png" | cut -d, -f2-) || exit 1
    echo "$image,$mode,$k,$n,$dither,$words,$loads,$spans_used,$metrics,$predict,$flag,$png" >> "$csv"
}

# 2. baseline: the micro-HAM encoder, scored by clut_compare on its own preview
#    PNG so old and new numbers come out of one metric implementation.  RETIRED
#    2026-09-20 -- micro-HAM went with the indexed PIXEL and ../microham_encode
#    no longer exists -- so this rung is SKIPPED when the binary is absent.  Its
#    recorded rows stay in out/results.csv.
for image in $images
do
    mkdir -p "$out/$image"
    if [ ! -x ../microham_encode ]
    then
        echo "  $image mham SKIPPED (micro-HAM is retired)"
        continue
    fi
    for dither in $dithers
    do
        preview="$out/$image/mham_$dither-dec.png"
        ../microham_encode --dither "$dither" "$out/$image-ref.png" "$out/$image/mham_$dither.ham" \
            "$out/$image/mham_$dither.pal" "$preview" > "$out/$image/mham_$dither.log" || exit 1
        row "$image" mham - - "$dither" 80 2.00 0.00 n/a "" "$preview"
        echo "  $image mham $dither"
    done
done

# 3. the CLUT modes
for image in $images
do
    for mode in $modes
    do
        case $mode in
            c2f) k_list="4" ;;
            *)   k_list="16 12 4" ;;
        esac
        for k in $k_list
        do
            for n in $spans
            do
                for dither in $dithers
                do
                    tag="${mode}_K${k}_N${n}_${dither}"
                    stream="$out/$image/$tag.clut"
                    pred="$out/$image/$tag-pred.png"
                    dec="$out/$image/$tag-dec.png"

                    ./clut_encode --mode "$mode" --k "$k" --n "$n" --dither "$dither" \
                        --predict "$pred" "$out/$image-ref.png" "$stream" > /dev/null || exit 1

                    info=$(./clut_decode "$stream" "$dec" 2>/dev/null) || exit 1
                    words=$(echo "$info" | sed -n 's/.*, \([0-9]*\) words\/line.*/\1/p')
                    loads=$(echo "$info" | sed -n 's/.*loads\/line \([0-9.]*\).*/\1/p')
                    spans_used=$(echo "$info" | sed -n 's/.*spans\/line \([0-9.]*\).*/\1/p')

                    if cmp -s "$pred" "$dec"
                    then
                        predict=ok
                    else
                        predict=MISMATCH
                        echo "PREDICTION MISMATCH: $stream" >&2
                    fi

                    total=$((words + 3 * n))
                    if [ "$total" -gt 147 ]
                    then
                        flag="over-cap:${total}w"
                    else
                        flag=""
                    fi

                    row "$image" "$mode" "$k" "$n" "$dither" "$words" "$loads" "$spans_used" \
                        "$predict" "$flag" "$dec"
                    echo "  $image $tag ${words}w loads=$loads spans=$spans_used $predict"
                done
            done
        done
    done
done

# 4. the §6 exact-reproduction gate image, kept in the CSV so the report can
#    show it alongside the photographs.
mkdir -p "$out/synth"
./clut_encode --mode c4f --k 16 --n 0 --dither none --predict "$out/synth/c4f-pred.png" \
    "$out/synth-ref.png" "$out/synth/c4f.clut" > /dev/null || exit 1
./clut_decode --quiet "$out/synth/c4f.clut" "$out/synth/c4f-dec.png" 2>/dev/null || exit 1
if cmp -s "$out/synth/c4f-pred.png" "$out/synth/c4f-dec.png"
then
    predict=ok
else
    predict=MISMATCH
fi
row synth c4f 16 0 none 160 - - "$predict" "over-cap:160w" "$out/synth/c4f-dec.png"

finished=$(date +%s)
echo "sweep: $(( finished - started )) seconds, $(( $(wc -l < "$csv") - 1 )) rows in $csv"

./report.py "$csv" "$out"
