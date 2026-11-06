#!/usr/bin/env python3
"""Remove one symbol instance from a KiCad 9 sheet, with what was generated on its pins.

  remove_symbol.py --sch io.kicad_sch --ref J3 [--dry-run]

Removed with the symbol block (every unit carrying that Reference), recognised
purely by geometry, as relabel_pins.py --relabel does:
  - a wire with one end exactly on a pin end and the other exactly 5.08 mm out
    along the pin axis (the stub);
  - at the stub's far end: global / local labels (their Intersheetrefs are part
    of the label block), no-connects, junctions, and power symbols (#PWR...)
    whose pin sits there;
  - on the pin end itself: global / local labels, no-connects and power symbols.
With --chain, a hand-drawn wire chain from a pin end (any length, with bends) is
taken instead of only the 5.08 mm stub, as long as it does not branch, touches
nothing mid-segment, and ends only in labels, no-connects or power symbols.
Anything else touching a pin end or a chain -- another wire, a hierarchical
label, another symbol's pin -- is an error naming the pin, and nothing is
written.  Rotated and mirrored instances are handled.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from label_cpld import fmt, parse_sexp, find_all, find_one, atom, lib_symbol_pins  # noqa: E402
from relabel_pins import _top_level_span, _key, _at_xy  # noqa: E402

STUB = 5.08
DIRS = {0: (1, 0), 90: (0, 1), 180: (-1, 0), 270: (0, -1)}      # lib (y up) unit vectors


def transform(lx, ly, rot, mirror):
    """Screen offset of lib point (lx, ly) for an instance at rot with optional mirror."""
    sx, sy = {0: (lx, -ly), 90: (-ly, -lx), 180: (-lx, ly), 270: (ly, lx)}[rot]
    if mirror == 'y':
        sx = -sx
    elif mirror == 'x':
        sy = -sy
    return sx, sy


def instance_pins(root, inst):
    """[(number, name, (x, y) pin end, (x, y) stub end)] for one placed unit."""
    lib_id = atom(find_one(inst, 'lib_id')[1])
    at = find_one(inst, 'at')
    x, y, rot = float(atom(at[1])), float(atom(at[2])), int(float(atom(at[3]))) % 360
    m = find_one(inst, 'mirror')
    mirror = atom(m[1]) if m is not None else None
    unit = find_one(inst, 'unit')
    unit = int(atom(unit[1])) if unit is not None else None
    out = []
    for number, name, _ptype, lx, ly, prot, _hidden in lib_symbol_pins(root, lib_id, unit):
        ox, oy = transform(lx, ly, rot, mirror)
        dx, dy = DIRS[prot % 360]                       # pin end -> body, lib coordinates
        ix, iy = transform(lx + dx, ly + dy, rot, mirror)
        ux, uy = ox - ix, oy - iy                       # screen unit vector away from the body
        out.append((number, name, (x + ox, y + oy), (x + ox + STUB * ux, y + oy + STUB * uy)))
    return out


def ref_of(sym):
    for p in find_all(sym, 'property'):
        if atom(p[1]) == 'Reference':
            return atom(p[2])
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sch', required=True)
    ap.add_argument('--ref', required=True)
    ap.add_argument('--chain', action='store_true',
                    help='also follow hand-drawn wires (any length, with bends) from a pin end, as long as the '
                         'chain does not branch and ends only in labels, no-connects or power symbols')
    ap.add_argument('--dry-run', action='store_true', help='report what would be removed, write nothing')
    args = ap.parse_args()

    text = open(args.sch).read()
    root = parse_sexp(text)[0]
    symbols = find_all(root, 'symbol')
    insts = [s for s in symbols if ref_of(s) == args.ref]
    if not insts:
        sys.exit(f"no symbol with Reference {args.ref} in {args.sch}")

    pin_at = {}             # key -> pin label
    stub_of = {}            # pin label -> stub end (x, y)
    for inst in insts:
        for number, name, pe, se in instance_pins(root, inst):
            tag = f"{number} ({name})"
            pin_at[_key(*pe)] = tag
            stub_of[tag] = se

    remove = []             # (uuid, description)
    errors = []
    wires = []
    for w in find_all(root, 'wire'):
        pts = [_key(float(atom(xy[1])), float(atom(xy[2]))) for xy in find_all(find_one(w, 'pts'), 'xy')]
        wires.append((atom(find_one(w, 'uuid')[1]), pts))

    # every connectable item, by point
    items = {}              # key -> [(kind, uuid, description)]

    def add(p, kind, uid, desc):
        items.setdefault(p, []).append((kind, uid, desc))
    for head in ('global_label', 'label', 'hierarchical_label'):
        for lb in find_all(root, head):
            add(_key(*_at_xy(lb)), head, atom(find_one(lb, 'uuid')[1]), f'{head} "{atom(lb[1])}"')
    for head in ('no_connect', 'junction'):
        for it in find_all(root, head):
            add(_key(*_at_xy(it)), head, atom(find_one(it, 'uuid')[1]), head)
    for sym in symbols:
        if sym in insts or find_one(sym, 'lib_id') is None:
            continue
        r = ref_of(sym) or '?'
        kind = 'power' if r.startswith('#PWR') or r.startswith('#FLG') else 'pin'
        for number, _name, pe, _se in instance_pins(root, sym):
            add(_key(*pe), kind, atom(find_one(sym, 'uuid')[1]),
                f"power symbol {r} ({atom(find_one(sym, 'lib_id')[1])})" if kind == 'power' else f"{r} pin {number}")
    wires_at = {}
    for uid, pts in wires:
        for p in pts:
            wires_at.setdefault(p, []).append(uid)
    wire_pts = dict(wires)

    def on_interior(p, uid):
        """True when point p lies strictly inside segment uid (KiCad joins it there)."""
        (ax, ay), (bx, by) = [tuple(map(float, q)) for q in wire_pts[uid][:2]]
        x, y = map(float, p)
        if abs((bx - ax) * (y - ay) - (by - ay) * (x - ax)) > 1e-6:
            return False
        return min(ax, bx) - 1e-6 <= x <= max(ax, bx) + 1e-6 and min(ay, by) - 1e-6 <= y <= max(ay, by) + 1e-6 \
            and p not in wire_pts[uid]

    every_point = set(items) | set(wires_at) | set(pin_at)

    def terminal(p, tag, where, allow_nc=True):
        """Remove what sits at p (labels, no-connects, junctions, power symbols); error on anything else."""
        for kind, uid, desc in items.get(p, []):
            if kind in ('global_label', 'label', 'power') or (kind in ('no_connect', 'junction') and allow_nc):
                remove.append((uid, f"pin {tag}: {desc} on the {where}"))
            else:
                errors.append(f"pin {tag}: {desc} on the {where}")

    for p, tag in pin_at.items():
        terminal(p, tag, 'pin end')
        for uid in wires_at.get(p, []):
            # walk the wire chain from the pin end
            chain = [uid]
            prev, cur = p, [q for q in wire_pts[uid] if q != p]
            if len(cur) != 1:
                errors.append(f"pin {tag}: odd wire {uid}")
                continue
            cur = cur[0]
            while True:
                if not args.chain and len(chain) == 1 and cur != _key(*stub_of[tag]):
                    errors.append(f"pin {tag}: a wire at the pin end that is not a generated "
                                  f"{fmt(STUB)} mm stub ({uid}); --chain follows hand-drawn wires")
                    break
                if cur in pin_at:
                    errors.append(f"pin {tag}: wire chain reaches another pin of {args.ref}")
                    break
                nxt = [w for w in wires_at.get(cur, []) if w not in chain]
                if len(nxt) == 1 and args.chain and not items.get(cur):
                    chain.append(nxt[0])
                    q = [z for z in wire_pts[nxt[0]] if z != cur]
                    prev, cur = cur, q[0]
                    continue
                if nxt:
                    errors.append(f"pin {tag}: another wire joins the chain at {cur} ({nxt})")
                    break
                terminal(cur, tag, f"wire end ({cur[0]}, {cur[1]})")
                for w in chain:
                    a, b = wire_pts[w][:2]
                    remove.append((w, f"pin {tag}: wire ({a[0]}, {a[1]})-({b[0]}, {b[1]})"))
                    for q in every_point:
                        if on_interior(q, w):
                            errors.append(f"pin {tag}: something at ({q[0]}, {q[1]}) touches wire {w} mid-segment")
                break

    if errors:
        sys.exit(f"{args.ref} in {args.sch}: refusing, nothing written:\n  " + "\n  ".join(sorted(set(errors))))

    for inst in insts:
        remove.append((atom(find_one(inst, 'uuid')[1]),
                       f"symbol {args.ref} ({atom(find_one(inst, 'lib_id')[1])}) at "
                       f"({fmt(float(atom(find_one(inst, 'at')[1])))}, {fmt(float(atom(find_one(inst, 'at')[2])))})"))
    seen = set()
    uniq = []
    for uid, desc in remove:
        if uid not in seen:
            seen.add(uid)
            uniq.append((uid, desc))
    print(f"{args.sch}: removing {len(uniq)} items for {args.ref}")
    for _, desc in uniq:
        print(f"  {desc}")
    if args.dry_run:
        return
    spans = sorted(_top_level_span(text, u) for u, _ in uniq)
    for (_, e0), (s1, _) in zip(spans, spans[1:]):
        if s1 < e0:
            sys.exit("internal error: overlapping items to remove")
    for s, e in reversed(spans):
        text = text[:s] + text[e:]
    open(args.sch, 'w').write(text)
    print(f"wrote {args.sch}")


if __name__ == '__main__':
    main()
