#!/usr/bin/env python3
"""report.py — results.csv -> results.md + index.html for the CLUT experiment.

Usage: report.py out/results.csv out

The markdown is for reading in a terminal or a diff; index.html is the thing
DESIGN.md §5 asks for: every image's reference next to the best variant of every
mode, at 1:1, with the mode, K, N, dither, words per line and both metrics in
the caption.  "Best" is shown twice, because the two metrics disagree on
purpose: box4 is the one that tracks what the eye integrates and it rewards
dithering, per-pixel is the one that punishes it.
"""

import csv
import html
import os
import sys

IMAGE_ORDER = ["maya2", "millie2", "adobe2", "oops2", "synth"]
MODE_ORDER = ["mham", "c4h", "c2f", "c4f", "attr"]

MODE_NOTE = {
    "mham": "baseline: today's 2bpp micro-HAM, 80 words/line",
    "c4h": "320 x 4bpp half rate, 80 words/line",
    "c2f": "640 x 2bpp full rate, 80 words/line, only CLUT entries 0..3 reachable",
    "c4f": "640 x 4bpp full rate, 160 words/line - OVER the 147-word line budget",
    "attr": "320 px as 40 cells of {fg4, bg4, pattern8}, 40 words/line",
}


def load(path):
    with open(path, newline="") as handle:
        rows = list(csv.DictReader(handle))
    for row in rows:
        row["per_pixel_mse"] = float(row["per_pixel_mse"])
        row["box4_mse"] = float(row["box4_mse"])
        row["psnr"] = float(row["psnr"])
    return rows


def label(row):
    if row["mode"] == "mham":
        return "mham %s" % row["dither"]
    return "%s K=%s N=%s %s" % (row["mode"], row["k"], row["n"], row["dither"])


def images_in(rows):
    seen = [r["image"] for r in rows]
    ordered = [i for i in IMAGE_ORDER if i in seen]
    return ordered + sorted(set(seen) - set(ordered))


def modes_in(rows):
    seen = {r["mode"] for r in rows}
    ordered = [m for m in MODE_ORDER if m in seen]
    return ordered + sorted(seen - set(ordered))


def best(rows, key):
    return min(rows, key=lambda r: r[key])


def summary_rows(subset, key):
    """The rows a best-per-mode table shows: the best variant of each mode, but
    every micro-HAM dither, since the baseline is what the reader is comparing
    against and its three dithers are not interchangeable."""
    out = []
    for mode in modes_in(subset):
        by_mode = [r for r in subset if r["mode"] == mode]
        if mode == "mham":
            order = {"none": 0, "ordered": 1, "fs": 2}
            out.extend(sorted(by_mode, key=lambda r: order.get(r["dither"], 9)))
        else:
            out.append(best(by_mode, key))
    return out


def write_markdown(rows, out_dir):
    path = os.path.join(out_dir, "results.md")
    with open(path, "w") as handle:
        handle.write("# CLUT photo experiment results\n\n")
        handle.write(
            "Metrics: `per_pixel` = mean squared R4G4B4 error per pixel, `box4` = mean\n"
            "squared 8-bit error of sliding 4x4 box means, `psnr` = 8-bit PSNR in dB.\n"
            "Lower is better for the first two, higher for PSNR.  Both of the first two\n"
            "are microham_encode's own metrics, recomputed by clut_compare so the\n"
            "baseline and the contenders come out of one implementation.\n\n"
            "`words` is the mode's PIXEL words per line; each span adds 3 VIDCMD words,\n"
            "so a line costs words + 3N against the 147-word budget (vidcmd-spec.md §4.5).\n"
            "`loads` and `spans` are what the encoder actually spent per line, not what K\n"
            "and N allowed.\n\n"
        )

        handle.write("## Best per mode, by 4x4-box error\n\n")
        handle.write("| image | mode | variant | words | loads/line | per_pixel | box4 | psnr |\n")
        handle.write("|---|---|---|---:|---:|---:|---:|---:|\n")
        for image in images_in(rows):
            for row in summary_rows([r for r in rows if r["image"] == image], "box4_mse"):
                handle.write(
                    "| %s | %s | %s | %s%s | %s | %.3f | %.2f | %.2f |\n"
                    % (image, row["mode"], label(row), row["words_per_line"],
                       " (over cap)" if row["flag"] else "", row["loads_per_line"],
                       row["per_pixel_mse"], row["box4_mse"], row["psnr"])
                )
        handle.write("\n## Best per mode, by per-pixel error\n\n")
        handle.write("| image | mode | variant | words | loads/line | per_pixel | box4 | psnr |\n")
        handle.write("|---|---|---|---:|---:|---:|---:|---:|\n")
        for image in images_in(rows):
            for row in summary_rows([r for r in rows if r["image"] == image], "per_pixel_mse"):
                handle.write(
                    "| %s | %s | %s | %s%s | %s | %.3f | %.2f | %.2f |\n"
                    % (image, row["mode"], label(row), row["words_per_line"],
                       " (over cap)" if row["flag"] else "", row["loads_per_line"],
                       row["per_pixel_mse"], row["box4_mse"], row["psnr"])
                )

        for image in images_in(rows):
            handle.write("\n## %s (every variant)\n\n" % image)
            handle.write(
                "| mode | K | N | dither | words | loads/line | spans/line | per_pixel | box4 "
                "| psnr | predict |\n"
            )
            handle.write("|---|---|---|---|---:|---:|---:|---:|---:|---:|---|\n")
            subset = [r for r in rows if r["image"] == image]
            subset.sort(key=lambda r: (MODE_ORDER.index(r["mode"]) if r["mode"] in MODE_ORDER
                                       else 9, r["box4_mse"]))
            for row in subset:
                handle.write(
                    "| %s | %s | %s | %s | %s%s | %s | %s | %.3f | %.2f | %.2f | %s |\n"
                    % (row["mode"], row["k"], row["n"], row["dither"], row["words_per_line"],
                       "*" if row["flag"] else "", row["loads_per_line"], row["spans_per_line"],
                       row["per_pixel_mse"], row["box4_mse"], row["psnr"], row["predict"])
                )
        handle.write("\n`*` = over the 147-word line budget (c4f's DMA bandwidth is NOT "
                     "verified).\n")
    return path


CSS = """
body { background: #181818; color: #e0e0e0; font: 14px/1.5 -apple-system, Helvetica, sans-serif;
       margin: 0 0 6em 0; padding: 0 2em; }
h1, h2 { font-weight: 600; }
h2 { border-top: 1px solid #333; padding-top: 1em; margin-top: 2em; }
figure { margin: 0 0 1.5em 0; display: inline-block; vertical-align: top; }
figure img { width: 640px; height: 480px; image-rendering: pixelated; display: block;
             border: 1px solid #333; }
figcaption { width: 640px; font-size: 12px; color: #b0b0b0; padding: 4px 0 0 0; }
figcaption b { color: #fff; }
.numbers { font-variant-numeric: tabular-nums; }
.flag { color: #ffb347; }
.grid { display: flex; flex-wrap: wrap; gap: 1.5em; }
table { border-collapse: collapse; font-size: 13px; font-variant-numeric: tabular-nums; }
th, td { border: 1px solid #333; padding: 2px 8px; text-align: right; }
th:first-child, td:first-child, th:nth-child(2), td:nth-child(2) { text-align: left; }
p.note { max-width: 60em; color: #b0b0b0; }
"""


def figure(row, out_dir, title):
    png = os.path.relpath(row["png"], out_dir)
    flag = (' <span class="flag">over the 147-word budget</span>' if row["flag"] else "")
    return (
        '<figure><img src="%s" alt="%s">\n'
        '<figcaption><b>%s</b><br>%s<br>'
        '<span class="numbers">%s pixel words + %d span words/line, %s CLUT loads/line, '
        '%s spans/line%s<br>'
        'per-pixel %.3f &middot; box4 %.2f &middot; PSNR %.2f dB</span>'
        "</figcaption></figure>\n"
        % (html.escape(png), html.escape(title), html.escape(title), html.escape(label(row)),
           row["words_per_line"], 3 * int(row["n"]) if row["n"].isdigit() else 0,
           row["loads_per_line"], row["spans_per_line"], flag,
           row["per_pixel_mse"], row["box4_mse"], row["psnr"])
    )


def write_html(rows, out_dir):
    path = os.path.join(out_dir, "index.html")
    with open(path, "w") as handle:
        handle.write("<!DOCTYPE html>\n<html><head><meta charset='utf-8'>\n")
        handle.write("<title>CLUT photo experiment</title>\n<style>%s</style></head><body>\n" % CSS)
        handle.write("<h1>CLUT photo experiment</h1>\n")
        handle.write(
            '<p class="note">Every picture is at 1:1 &mdash; 640x480, unscaled, nearest '
            "neighbour if your browser zooms &mdash; because 1:1 is the only size this "
            "hardware has.  For each image: the reference, today&rsquo;s micro-HAM, and the "
            "best variant of each candidate mode by each of the two metrics (they disagree on "
            "purpose: <b>box4</b> rewards dithering because a 4x4 mean is blind to how "
            "violently the pixels inside it disagree, <b>per-pixel</b> punishes it).  "
            "<b>c4f is flagged</b>: at 160 pixel words per line it is over the 147-word "
            "ENGINE budget and its DMA bandwidth is not verified.</p>\n"
        )

        for image in images_in(rows):
            subset = [r for r in rows if r["image"] == image]
            handle.write("<h2>%s</h2>\n" % html.escape(image))
            handle.write('<div class="grid">\n')
            reference = os.path.relpath(os.path.join(out_dir, "%s-ref.png" % image), out_dir)
            handle.write(
                '<figure><img src="%s" alt="reference">\n<figcaption><b>reference</b><br>'
                "640x480 8-bit sRGB, the input both encoders were given</figcaption></figure>\n"
                % html.escape(reference)
            )
            for mode in modes_in(subset):
                by_mode = [r for r in subset if r["mode"] == mode]
                box = best(by_mode, "box4_mse")
                pixel = best(by_mode, "per_pixel_mse")
                handle.write(figure(box, out_dir, "%s - best box4" % mode))
                if pixel["png"] != box["png"]:
                    handle.write(figure(pixel, out_dir, "%s - best per-pixel" % mode))
            handle.write("</div>\n")

            handle.write("<h3>%s, every variant (sorted by box4)</h3>\n" % html.escape(image))
            handle.write("<table><tr><th>mode</th><th>K</th><th>N</th><th>dither</th>"
                         "<th>words</th><th>loads/line</th><th>spans/line</th>"
                         "<th>per-pixel</th><th>box4</th><th>PSNR</th><th>predict</th></tr>\n")
            subset.sort(key=lambda r: r["box4_mse"])
            for row in subset:
                handle.write(
                    "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s%s</td><td>%s</td>"
                    "<td>%s</td><td>%.3f</td><td>%.2f</td><td>%.2f</td><td>%s</td></tr>\n"
                    % (row["mode"], row["k"], row["n"], row["dither"], row["words_per_line"],
                       "*" if row["flag"] else "", row["loads_per_line"], row["spans_per_line"],
                       row["per_pixel_mse"], row["box4_mse"], row["psnr"], row["predict"])
                )
            handle.write("</table>\n")

        handle.write("<p class='note'>Mode key: " + "; ".join(
            "<b>%s</b> %s" % (m, html.escape(MODE_NOTE.get(m, ""))) for m in modes_in(rows)
        ) + ".</p>\n")
        handle.write("</body></html>\n")
    return path


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("Usage: report.py <results.csv> <out-dir>\n")
        return 1
    rows = load(sys.argv[1])
    out_dir = sys.argv[2]
    print(write_markdown(rows, out_dir))
    print(write_html(rows, out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
