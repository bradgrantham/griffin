#!/usr/bin/env python3
"""Lay out the root sheet's hierarchical sheet blocks on a grid, in a given order.

The root gets (paper "A3").  Every (sheet ...) block whose file is not in
--order is removed (and printed).  The remaining blocks, plus a new block for
every ordered file the root does not reference yet, are re-gridded row-major on
a 6-column x 4-row grid of 40 x 20 boxes at x = 25.4 + 63.5*col,
y = 25.4 + 38.1*row, and their pages are renumbered 2.. in order.  An ordered
file that does not exist is created as a blank KiCad 9 USLetter sheet.  The
project file's "sheets" list is regenerated: root first, then the order.

--order items are file basenames without .kicad_sch; the sheet name is the
uppercase basename ('-' -> '_') unless given as NAME=file or listed in NAMES
below.  An existing block whose Sheetname differs is renamed (and printed).

Example:
  layout_root.py --root board.kicad_sch --order power-clock,cpu,rom,ram,glue,compact-flash,io,serial,rtc,ports,panel,debug-header,engine,pixel,compositor,vga,audio,audio-fifo,audio-dac,audio-amp
"""

import argparse
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from new_cpld_sheet import sheet_block, blank_sheet_text, fmt  # noqa: E402
from place_fifo import block_span  # noqa: E402

NAMES = {
    'power-clock': 'POWER_CLOCK_RESET',
    'compact-flash': 'COMPACT_FLASH',
    'debug-header': 'DEBUG_HEADER',
    'vga': 'VGA_OUT',
    'audio-fifo': 'AUDIO_FIFO',
    'audio-dac': 'AUDIO_DAC',
    'audio-amp': 'AUDIO_AMP',
}
COLS, ROWS = 6, 4
X0, Y0, DX, DY = 25.4, 25.4, 63.5, 38.1
W, H = 40, 20


def sheet_blocks(text):
    """[(start, end, uuid, name, file)] for every top-level (sheet ...) block."""
    out = []
    pos = 0
    while True:
        span = block_span(text, r'^\t\(sheet\n', pos)
        if not span:
            return out
        blk = text[span[0]:span[1]]
        out.append((span[0], span[1], re.search(r'^\t\t\(uuid "([0-9a-f-]+)"\)', blk, re.M).group(1),
                    re.search(r'\(property "Sheetname" "([^"]*)"', blk).group(1),
                    re.search(r'\(property "Sheetfile" "([^"]*)"', blk).group(1)))
        pos = span[1]


def regrid(blk, name, x, y, page):
    """Move an existing block: position, size, field positions, name and page."""
    blk = re.sub(r'^\t\t\(at [^)]*\)', f'\t\t(at {fmt(x)} {fmt(y)})', blk, count=1, flags=re.M)
    blk = re.sub(r'^\t\t\(size [^)]*\)', f'\t\t(size {fmt(W)} {fmt(H)})', blk, count=1, flags=re.M)
    blk = re.sub(r'(\(property "Sheetname" )"[^"]*"(\n\t\t\t)\(at [^)]*\)',
                 lambda m: f'{m.group(1)}"{name}"{m.group(2)}(at {fmt(x)} {fmt(y - 0.7116)} 0)', blk, count=1)
    blk = re.sub(r'(\(property "Sheetfile" "[^"]*"\n\t\t\t)\(at [^)]*\)',
                 lambda m: f'{m.group(1)}(at {fmt(x)} {fmt(y + H + 3.4446)} 0)', blk, count=1)
    blk, n = re.subn(r'\(page "[^"]*"\)', f'(page "{page}")', blk)
    if n != 1:
        sys.exit(f"sheet block for {name}: {n} page entries")
    return blk


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', required=True)
    ap.add_argument('--order', required=True, help='comma list of file basenames or NAME=basename')
    args = ap.parse_args()

    order = []
    for item in args.order.split(','):
        name, _, base = item.rpartition('=')
        base = base.strip()
        order.append((name or NAMES.get(base, base.upper().replace('-', '_')), base + '.kicad_sch'))
    if len(order) > COLS * ROWS:
        sys.exit(f"{len(order)} sheets do not fit the {COLS}x{ROWS} grid")
    if len({f for _, f in order}) != len(order):
        sys.exit("a file appears twice in --order")

    rdir = os.path.dirname(os.path.abspath(args.root))
    text = open(args.root).read()
    root_uuid = re.search(r'^\t\(uuid "([0-9a-f-]+)"\)', text, re.M).group(1)
    text, n = re.subn(r'^\t\(paper "[^"]*"\)', '\t(paper "A3")', text, count=1, flags=re.M)
    if n != 1:
        sys.exit("no (paper ...) line in the root")

    blocks = sheet_blocks(text)
    by_file = {}
    for s, e, u, name, f in blocks:
        if f in by_file:
            sys.exit(f"{f} is referenced twice on the root")
        by_file[f] = (text[s:e], u, name)
    wanted = {f for _, f in order}
    for s, e, u, name, f in blocks:
        if f not in wanted:
            print(f"removed sheet {name} ({f}, uuid {u})")

    new_blocks = []
    pro_sheets = [(root_uuid, 'Root')]
    for i, (name, f) in enumerate(order):
        col, row = i % COLS, i // COLS
        x, y = X0 + DX * col, Y0 + DY * row
        page = str(i + 2)
        if f in by_file:
            blk, u, old_name = by_file[f]
            if old_name != name:
                print(f"renamed sheet {old_name} -> {name} ({f})")
            blk = regrid(blk, name, x, y, page)
        else:
            u = str(uuid.uuid4())
            blk = sheet_block(u, root_uuid, name, f, x, y, W, H, page).rstrip('\n')
            print(f"added sheet {name} ({f}, uuid {u})")
        path = os.path.join(rdir, f)
        if not os.path.exists(path):
            open(path, 'w').write(blank_sheet_text())
            print(f"created blank {f}")
        new_blocks.append(blk)
        pro_sheets.append((u, name))

    # Replace all old blocks by the ordered ones, at the position of the first old block
    # (or before sheet_instances when the root had none).
    if blocks:
        insert_at = blocks[0][0]
        for s, e, *_ in reversed(blocks):
            # drop the block and its trailing newline
            text = text[:s] + text[e + 1 if text[e:e + 1] == '\n' else e:]
    else:
        insert_at = text.index('\t(sheet_instances')
    text = text[:insert_at] + '\n'.join(new_blocks) + '\n' + text[insert_at:]
    open(args.root, 'w').write(text)

    pro = os.path.splitext(args.root)[0] + '.kicad_pro'
    if os.path.exists(pro):
        ptext = open(pro).read()
        m = re.search(r'\n  "sheets": \[(?:.|\n)*?\n  \],?\n', ptext)
        if not m:
            sys.exit(f"no \"sheets\" list in {pro}")
        entries = ',\n'.join(f'    [\n      "{u}",\n      "{n}"\n    ]' for u, n in pro_sheets)
        comma = ',' if m.group(0).rstrip('\n').endswith(',') else ''
        ptext = ptext[:m.start()] + f'\n  "sheets": [\n{entries}\n  ]{comma}\n' + ptext[m.end():]
        open(pro, 'w').write(ptext)

    print(f"{args.root}: A3, {len(order)} sheet blocks, pages 2..{len(order) + 1}")


if __name__ == '__main__':
    main()
