#!/usr/bin/env python3
"""Place parts on a KiCad 9 sheet from a spec module, wiring each listed pin.

The spec (a Python file, e.g. sheets/rtc_spec.py) defines:

  PARTS = [dict(ref='U40', lib='Timer_RTC', name='DS1302', value='DS1302',
                footprint='Package_DIP:DIP-8_W7.62mm', at=(101.6, 76.2), rot=0,
                unit=1,                      # optional; one entry per unit of a multi-unit part
                datasheet='~', description='',   # optional
                pins={'1': ('power', '+5V'), '5': ('global', '~{RTC_CE}', 'input'), ...}), ...]
  PWR_START = 300                            # first #PWR number for the power symbols
  TEXTS = [('note', x, y), ...]              # optional

Pin actions (keys are lib pin numbers, as strings):
  ('global', NET, shape)   5.08 mm stub, global label; shape input|output|bidirectional|passive
                           (tri_state too)
  ('local', NET)           5.08 mm stub, local label
  ('power', '+5V'|'GND')   power symbol; on the pin end for a power pin, on a stub otherwise,
                           its glyph pointing away from the body
  'nc'                     no-connect flag
  ('series', REF, VALUE, NET, shape)
                           pin -> 2.54 wire -> two-pin Device:R along the pin axis -> 2.54 wire
                           -> global label NET.  When the pin is passive, the pin-side net also
                           gets a local label REF_PIN (KiCad's netlist drops an unnamed net
                           between two passive pins)
  ('testpoint', REF, NET[, shape])
                           stub, global label NET (default shape passive) and a
                           Connector:TestPoint (TestPoint_Pad_D1.5mm) on the same stub end

Stubs run along the pin axis away from the body for any pin direction and any
symbol rotation (0/90/180/270, KiCad's counter-clockwise convention: lib pin
(lx, ly) lands at (x + lx, y - ly) at 0, (x - ly, y - lx) at 90, (x - lx, y + ly)
at 180 and (x + ly, y + lx) at 270).  Unlisted pins are reported, not wired.

Every reference (parts, series resistors, test points, #PWR from PWR_START) must
be free on every sheet in the directory; the script refuses otherwise.  Lib
nicknames resolve through the sheet directory's sym-lib-table first, then
KiCad's libraries; derived symbols are flattened.

Example:  place_parts.py --sch rtc.kicad_sch --spec sheets/rtc_spec.py --dry-run
"""

import argparse
import importlib.util
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from label_cpld import fmt, wire_block, label_block, nc_block, parse_sexp, find_all, find_one, atom  # noqa: E402
from place_fifo import (ensure_lib_symbol, lib_pins, instance_block, prop, power_block,  # noqa: E402
                        two_pin_block, local_label_block, testpoint_block, used_references, sheet_path,
                        RES, TESTPOINT)
from place_headers import text_block  # noqa: E402

STUB = 5.08
SHAPES = ('input', 'output', 'bidirectional', 'passive', 'tri_state')
POWER_TYPES = ('power_in', 'power_out')


# ----------------------------------------------------------------------------
# geometry
# ----------------------------------------------------------------------------

def rotate(lx, ly, rot):
    """Screen offset of lib point (lx, ly) (y up) for a symbol at rotation rot (y down)."""
    return {0: (lx, -ly), 90: (-ly, -lx), 180: (-lx, ly), 270: (ly, lx)}[rot]


def direction(angle):
    """Screen unit vector for a y-up angle in degrees (multiples of 90)."""
    return round(math.cos(math.radians(angle))), -round(math.sin(math.radians(angle)))


def body_box(sheet, lib_id, unit, rot):
    """Screen-offset bounding box (x0, y0, x1, y1) of a unit's graphics, or None."""
    root = parse_sexp(sheet)[0]
    sym = [s for s in find_all(find_one(root, 'lib_symbols'), 'symbol') if atom(s[1]) == lib_id][0]
    pts = []

    def xy(node):
        return float(atom(node[1])), float(atom(node[2]))
    for sub in find_all(sym, 'symbol'):
        m = re.search(r'_(\d+)_(\d+)$', atom(sub[1]))
        if m and (int(m.group(1)) not in (0, unit) or int(m.group(2)) not in (0, 1)):
            continue
        for g in sub[2:]:
            if not isinstance(g, list) or not g:
                continue
            if g[0] == 'rectangle':
                pts += [xy(find_one(g, 'start')), xy(find_one(g, 'end'))]
            elif g[0] in ('polyline', 'bezier'):
                pts += [xy(p) for p in find_all(find_one(g, 'pts'), 'xy')]
            elif g[0] == 'circle':
                cx, cy = xy(find_one(g, 'center'))
                r = float(atom(find_one(g, 'radius')[1]))
                pts += [(cx - r, cy - r), (cx + r, cy + r)]
            elif g[0] == 'arc':
                pts += [xy(find_one(g, k)) for k in ('start', 'mid', 'end') if find_one(g, k)]
    if not pts:
        return None
    sp = [rotate(x, y, rot) for x, y in pts]
    return (min(p[0] for p in sp), min(p[1] for p in sp), max(p[0] for p in sp), max(p[1] for p in sp))


def fix_label_justify(blk, angle):
    """label_block justifies right only at 180; a label pointing down (270) is right-justified too."""
    return blk.replace('(justify left)', '(justify right)', 1) if angle == 270 else blk


def power_symbol(kind, x, y, angle, ref, path):
    """Power glyph at (x, y) pointing along `angle` (y-up degrees), Value past the tip, horizontal."""
    glyph = 90 if kind == '+5V' else 270          # where the glyph points at rotation 0
    rot = (angle - glyph) % 360
    blk = power_block(kind, x, y, rot, ref, path)
    if rot in (90, 180):                         # power_block's value placement covers 0 and 270
        dx, dy = direction(angle)
        if rot == 180:
            vx, vy, va = x, y + 5.08 * dy, 0
        else:
            vx, vy, va = x + 7.62 * dx, y, 90
        new = prop('Value', kind, vx, vy, angle=va)
        blk = re.sub(r'\t\t\(property "Value" (?:[^\n]*\n)*?\t\t\)\n', lambda m: new, blk, count=1)
    return blk


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------

def load_spec(path):
    spec = importlib.util.spec_from_file_location('placement_spec', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sch', required=True)
    ap.add_argument('--spec', required=True, help='spec module defining PARTS, PWR_START and optional TEXTS')
    ap.add_argument('--dry-run', action='store_true', help='report the placement, write nothing')
    args = ap.parse_args()

    spec = load_spec(args.spec)
    parts = spec.PARTS
    texts = getattr(spec, 'TEXTS', [])
    pwr_next = getattr(spec, 'PWR_START', None)
    project_dir = os.path.dirname(os.path.abspath(args.sch))

    sheet = open(args.sch).read()
    path = sheet_path(args.sch)
    used = used_references(args.sch)

    # references: a part ref may repeat only across distinct units of one symbol
    seen = {}
    extra = []
    for p in parts:
        key = (p['ref'], p.get('unit', 1))
        if key in seen:
            sys.exit(f"{p['ref']} unit {key[1]} listed twice")
        if any(r == p['ref'] and (q['lib'], q['name']) != (p['lib'], p['name']) for (r, _), q in seen.items()):
            sys.exit(f"{p['ref']} names two different symbols")
        seen[key] = p
        for act in p['pins'].values():
            if isinstance(act, tuple) and act[0] in ('series', 'testpoint'):
                extra.append(act[1])
    wanted = sorted({r for r, _ in seen} | set(extra))
    dup = sorted({r for r in extra if extra.count(r) > 1} | ({r for r, _ in seen} & set(extra)))
    if dup:
        sys.exit(f"references used twice in the spec: {dup}")
    clash = sorted(set(wanted) & used)
    if clash:
        sys.exit(f"references already in use: {clash}")

    # library symbols
    need = {(p['lib'], p['name']) for p in parts}
    acts = [a for p in parts for a in p['pins'].values() if isinstance(a, tuple)]
    if any(a[0] == 'series' for a in acts):
        need.add((RES['lib'], RES['name']))
    if any(a[0] == 'testpoint' for a in acts):
        need.add((TESTPOINT['lib'], TESTPOINT['name']))
    for a in acts:
        if a[0] == 'power':
            if a[1] not in ('+5V', 'GND'):
                sys.exit(f"power action {a}: only +5V and GND")
            need.add(('power', a[1]))
    for lib, name in sorted(need):
        sheet = ensure_lib_symbol(sheet, lib, name, project_dir)

    pwr_used = []

    def next_pwr():
        nonlocal pwr_next
        if pwr_next is None:
            sys.exit("spec has power actions but no PWR_START")
        r = f'#PWR{pwr_next:04d}'
        pwr_next += 1
        pwr_used.append(r)
        return r

    out = []
    report = []
    for p in parts:
        lib_id = f"{p['lib']}:{p['name']}"
        unit = p.get('unit', 1)
        rot = p.get('rot', 0)
        if rot not in (0, 90, 180, 270):
            sys.exit(f"{p['ref']}: rot must be 0/90/180/270")
        x, y = p['at']
        pins = {n: (n, nm, lx, ly, pr, ptype, length, hidden)
                for n, nm, lx, ly, pr, ptype, length, hidden in lib_pins(sheet, lib_id, unit=unit, full=True)}
        unknown = sorted(set(p['pins']) - set(pins))
        if unknown:
            sys.exit(f"{p['ref']} unit {unit}: pins {unknown} are not on {lib_id} unit {unit}")
        unlisted = sorted(set(pins) - set(p['pins']), key=lambda s: (len(s), s))
        if unlisted:
            print(f"warning: {p['ref']} unit {unit}: pins {unlisted} not listed, left unwired")

        # fields: from the unit's graphics box (pin ends if it has none)
        ends = {n: rotate(v[2], v[3], rot) for n, v in pins.items()}
        box = body_box(sheet, lib_id, unit, rot)
        if box is None:
            box = (min(e[0] for e in ends.values()), min(e[1] for e in ends.values()),
                   max(e[0] for e in ends.values()), max(e[1] for e in ends.values()))
        bx0, by0, bx1, by1 = (x + box[0], y + box[1], x + box[2], y + box[3])
        axes = {(pins[n][4] + rot) % 180 for n in pins}      # 0: horizontal pin, 90: vertical pin
        a = 90 if rot in (90, 270) else 0
        if axes == {90} and bx1 - bx0 < 7.62:                # top/bottom pins only: fields to the right
            # rotated: KiCad transforms a field's justification with the symbol, so
            # centre the text and shift it right by about half its width
            def fx(text):
                return bx1 + 1.27 + (0 if rot == 0 else 0.55 * len(text))
            fy = (by0 + by1) / 2
            j = 'left' if rot == 0 else None
            fields = [prop('Reference', p['ref'], fx(p['ref']), fy - 1.27, justify=j, angle=a),
                      prop('Value', p['value'], fx(p['value']), fy + 1.27, justify=j, angle=a)]
        elif rot == 0:                                       # above-left and below-left of the body
            fields = [prop('Reference', p['ref'], bx0, by0 - 1.905, justify='left'),
                      prop('Value', p['value'], bx0, by1 + 1.905, justify='left')]
        else:                                                # rotated: centred above and below
            cx = (bx0 + bx1) / 2
            fields = [prop('Reference', p['ref'], cx, by0 - 1.905, angle=a),
                      prop('Value', p['value'], cx, by1 + 1.905, angle=a)]
        fields += [prop('Footprint', p.get('footprint', ''), x, y, hide=True, angle=a),
                   prop('Datasheet', p.get('datasheet', '~'), x, y, hide=True, angle=a),
                   prop('Description', p.get('description', ''), x, y, hide=True, angle=a)]
        out.append(instance_block(lib_id, x, y, rot, p['ref'], fields, list(pins), path, unit))

        for num, act in p['pins'].items():
            _, pname, lx, ly, prot, ptype, length, hidden = pins[num]
            ox, oy = rotate(lx, ly, rot)
            px, py = x + ox, y + oy
            angle = (prot + 180 + rot) % 360              # away from the body, y-up degrees
            dx, dy = direction(angle)
            ex, ey = px + STUB * dx, py + STUB * dy
            kind = act if isinstance(act, str) else act[0]
            report.append(f"  {p['ref']}.{num:<3} {pname:<12} {act}")
            if kind == 'nc':
                out.append(nc_block(px, py))
            elif kind == 'global':
                _, net, shape = act
                if shape not in SHAPES:
                    sys.exit(f"{p['ref']}.{num}: shape {shape!r} not one of {SHAPES}")
                out += [wire_block(px, py, ex, ey), fix_label_justify(label_block(net, shape, ex, ey, angle), angle)]
            elif kind == 'local':
                out += [wire_block(px, py, ex, ey), local_label_block(act[1], ex, ey, angle)]
            elif kind == 'power':
                if ptype in POWER_TYPES:
                    out.append(power_symbol(act[1], px, py, angle, next_pwr(), path))
                else:
                    out += [wire_block(px, py, ex, ey), power_symbol(act[1], ex, ey, angle, next_pwr(), path)]
            elif kind == 'series':
                _, rref, rval, net, shape = act
                if shape not in SHAPES:
                    sys.exit(f"{p['ref']}.{num}: shape {shape!r} not one of {SHAPES}")
                x1, y1 = px + 2.54 * dx, py + 2.54 * dy
                rc = (x1 + 3.81 * dx, y1 + 3.81 * dy)
                x2, y2 = x1 + 7.62 * dx, y1 + 7.62 * dy
                x3, y3 = x2 + 2.54 * dx, y2 + 2.54 * dy
                out += [wire_block(px, py, x1, y1),
                        two_pin_block(RES, rc[0], rc[1], 90 if dy == 0 else 0, rref, rval, path),
                        wire_block(x2, y2, x3, y3),
                        fix_label_justify(label_block(net, shape, x3, y3, angle), angle)]
                if ptype == 'passive':
                    out.append(local_label_block(f"{rref}_{p['ref']}{num}", px + 1.27 * dx, py + 1.27 * dy,
                                                 angle if angle in (0, 180) else 0))
            elif kind == 'testpoint':
                tref, net = act[1], act[2]
                shape = act[3] if len(act) > 3 else 'passive'
                out += [wire_block(px, py, ex, ey), fix_label_justify(label_block(net, shape, ex, ey, angle), angle),
                        testpoint_block(ex, ey, dx == 0, tref, path)]
            else:
                sys.exit(f"{p['ref']}.{num}: unknown action {act!r}")

    for t, tx, ty in texts:
        out.append(text_block(t.replace('"', '\\"'), tx, ty))

    clash = sorted(set(pwr_used) & used)
    if clash:
        sys.exit(f"power references already in use: {clash}")

    print(f"{args.sch}: {len(seen)} symbol units ({len({r for r, _ in seen})} parts), "
          f"{len(extra)} series/test-point parts, power "
          + (f"{pwr_used[0]}..{pwr_used[-1]}" if pwr_used else 'none'))
    print('\n'.join(report))
    if args.dry_run:
        return
    i = sheet.rstrip().rfind(')')
    sheet = sheet[:i] + ''.join(out) + sheet[i:]
    open(args.sch, 'w').write(sheet)
    print(f"wrote {args.sch}")


if __name__ == '__main__':
    main()
