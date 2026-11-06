#!/usr/bin/env python3
"""Label chosen pins of any symbol in a KiCad 9 schematic.

Each --pin puts, on that pin of the symbol instance --ref:
  N=NET:shape   a 5.08 mm wire stub and a global label (shape input | output |
                bidirectional | passive) at the stub end
  N=NET:local   a stub and a local (sheet-scope) label
  N=NC          a no-connect flag on the pin end
Pins not mentioned are left untouched, so a subset can be labeled (e.g. the
DUART's OP pins).  The stub runs along the pin axis away from the body; pin
rotations 0, 90, 180 and 270 are handled.  The symbol instance itself must be
unrotated and unmirrored.

An existing no-connect on a mentioned pin is replaced.  An existing wire or
label on a mentioned pin end is an error (nothing is written), unless
--relabel: then what a previous run (or label_cpld.py) generated is stripped
first, recognised purely by geometry -- the wire with one end exactly on the
pin end and the other exactly 5.08 mm out along the pin axis, the global or
local label exactly at that stub end, a global or local label placed directly
on the pin end (no stub), and a no-connect on the pin end (or at the far end
of such a stub, a hand-made "dead stub").  Any
other wire or label at the pin end (or anything else joined to the stub end)
is an error naming the pin, and nothing is written.

Example (free the DUART OP4..OP7 pins):
  relabel_pins.py --sch io.kicad_sch --ref U27 --relabel \\
      --pin 27=NC --pin 14=NC --pin 26=NC --pin 15=NC
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from label_cpld import (fmt, wire_block, label_block, local_label_block, nc_block,  # noqa: E402
                        parse_sexp, find_all, find_one, atom, symbol_instance, lib_symbol_pins)

STUB = 5.08
GLOBAL_SHAPES = ('input', 'output', 'bidirectional', 'passive')
NON_IO_TYPES = ('power_in', 'power_out', 'passive', 'no_connect')

# lib pin rotation -> (stub direction in schematic coordinates, label angle)
# rot 0: the pin points right into the body, so the stub runs left, etc.
# (Library y is up, schematic y is down.)
PIN_DIRECTIONS = {
    0: ((-1, 0), 180),
    180: ((1, 0), 0),
    90: ((0, 1), 270),
    270: ((0, -1), 90),
}


def parse_pin_spec(spec):
    """'N=NET:shape' | 'N=NET:local' | 'N=NC' -> (N, (kind, net, shape))."""
    num, sep, rest = spec.partition('=')
    num = num.strip()
    if not sep or not num:
        sys.exit(f"--pin {spec}: expected N=NET:shape, N=NET:local or N=NC")
    num = int(num) if num.isdigit() else num        # pin numbers like S1 stay strings
    if rest == 'NC':
        return num, ('nc', None, None)
    net, sep, shape = rest.rpartition(':')
    if not sep or not net:
        sys.exit(f"--pin {spec}: expected N=NET:shape, N=NET:local or N=NC")
    if shape == 'local':
        return num, ('local', net, None)
    if shape not in GLOBAL_SHAPES:
        sys.exit(f"--pin {spec}: shape must be one of {list(GLOBAL_SHAPES)} or local")
    return num, ('global', net, shape)


def _key(x, y):
    return (fmt(x), fmt(y))


def _at_xy(node):
    a = find_one(node, 'at')
    return float(atom(a[1])), float(atom(a[2]))


def _top_level_span(text, uid):
    """(start, end) of the top-level schematic item whose own uuid is uid."""
    needle = f'(uuid "{uid}")'
    pos = text.find(needle)
    if pos < 0 or text.find(needle, pos + 1) >= 0:
        sys.exit(f"uuid {uid} not found exactly once in the file text")
    start = text.rfind('\n\t(', 0, pos) + 1
    depth = 0
    i = start
    in_str = False
    while i < len(text):
        c = text[i]
        if in_str:
            if c == '\\':
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                end = i + 1
                if text[end:end + 1] == '\n':
                    end += 1
                return start, end
        i += 1
    sys.exit(f"unterminated item around uuid {uid}")


def symbol_pin_ends(root, ref):
    """Return (instance, lib_id, {pin number str: (x, y, rot, name, type, hidden)})."""
    inst = symbol_instance(root, ref)
    lib_id = atom(find_one(inst, 'lib_id')[1])
    at = find_one(inst, 'at')
    sx, sy, srot = float(atom(at[1])), float(atom(at[2])), int(float(atom(at[3])))
    if srot != 0 or find_one(inst, 'mirror') is not None:
        sys.exit(f"{ref} is rotated or mirrored; only rotation 0 is supported")
    unit = find_one(inst, 'unit')
    unit = int(atom(unit[1])) if unit is not None else None
    ends = {}
    for number, name, ptype, lx, ly, rot, hidden in lib_symbol_pins(root, lib_id, unit):
        if number in ends and ends[number][:2] != (sx + lx, sy - ly):
            sys.exit(f"{ref}: pin {number} appears twice at different places")
        ends[number] = (sx + lx, sy - ly, rot % 360, name, ptype, hidden)
    return inst, lib_id, ends


def io_pin_numbers(ends):
    """Visible, non-power, non-passive pins: the ones label_cpld treats as I/O."""
    return sorted(((int(n) if n.isdigit() else n) for n, e in ends.items()
                   if not e[5] and e[4] not in NON_IO_TYPES), key=_pin_order)


def _pin_order(num):
    s = str(num)
    return (0, int(s), '') if s.isdigit() else (1, 0, s)


def apply(sch, ref, assign, relabel=False, dry_run=False):
    """Label pins of symbol ref in sch.  assign: {pin int: (kind, net, shape)}
    with kind 'global' | 'local' | 'nc'.  Returns the generated text."""
    text = open(sch).read()
    root = parse_sexp(text)[0]
    inst, lib_id, ends = symbol_pin_ends(root, ref)

    bad = sorted((n for n in assign if str(n) not in ends), key=_pin_order)
    if bad:
        sys.exit(f"pins {bad} are not pins of {ref} ({lib_id})")
    hidden = sorted((n for n in assign if ends[str(n)][5]), key=_pin_order)
    if hidden:
        sys.exit(f"pins {hidden} of {ref} ({lib_id}) are hidden")

    # Geometry of every mentioned pin: pin end, stub end, label angle.
    geo = {}
    for num in assign:
        x, y, rot, _, _, _ = ends[str(num)]
        if rot not in PIN_DIRECTIONS:
            sys.exit(f"pin {num} rotation {rot} not supported")
        (dx, dy), angle = PIN_DIRECTIONS[rot]
        geo[num] = ((x, y), (x + STUB * dx, y + STUB * dy), angle)
    pin_at = {_key(*g[0]): n for n, g in geo.items()}
    stub_at = {_key(*g[1]): n for n, g in geo.items()}

    remove = []         # uuids of items to strip
    errors = []
    wires = []
    for w in find_all(root, 'wire'):
        pts = [(float(atom(xy[1])), float(atom(xy[2]))) for xy in find_all(find_one(w, 'pts'), 'xy')]
        wires.append((atom(find_one(w, 'uuid')[1]), [_key(*p) for p in pts]))
    labels = []
    for head in ('global_label', 'label', 'hierarchical_label'):
        for lb in find_all(root, head):
            labels.append((head, atom(lb[1]), atom(find_one(lb, 'uuid')[1]), _key(*_at_xy(lb))))

    stubs = {}          # pin -> stub wire uuid
    for uid, pts in wires:
        for i, p in enumerate(pts):
            if p not in pin_at:
                continue
            num = pin_at[p]
            other = pts[1 - i] if len(pts) == 2 else None
            if relabel and other == _key(*geo[num][1]) and num not in stubs:
                stubs[num] = uid
            else:
                errors.append(f"pin {num}: a wire at the pin end that is not a generated "
                              f"{fmt(STUB)} mm stub ({uid})")
    for num, uid in stubs.items():
        remove.append(uid)
        # nothing else may join the stub end
        sk = _key(*geo[num][1])
        for wuid, pts in wires:
            if wuid != uid and sk in pts:
                errors.append(f"pin {num}: another wire joins the stub end ({wuid})")
    for head, name, uid, p in labels:
        if p in pin_at:
            if relabel and head != 'hierarchical_label':
                remove.append(uid)      # stub-less form: the label placed right on the pin end
            else:
                errors.append(f"pin {pin_at[p]}: {head} \"{name}\" sits on the pin end")
        elif p in stub_at and stub_at[p] in stubs:
            if head == 'hierarchical_label':
                errors.append(f"pin {stub_at[p]}: hierarchical_label \"{name}\" at the stub end")
            else:
                remove.append(uid)
    stale_nc = []
    for nc in find_all(root, 'no_connect'):
        p = _key(*_at_xy(nc))
        if p in pin_at:
            stale_nc.append(atom(find_one(nc, 'uuid')[1]))
        elif p in stub_at and stub_at[p] in stubs:
            remove.append(atom(find_one(nc, 'uuid')[1]))    # a dead stub: no-connect at its far end
    if errors:
        what = "--relabel cannot strip" if relabel else "existing objects on pin ends, refusing (use --relabel?)"
        sys.exit(f"{ref}: {what}:\n  " + "\n  ".join(sorted(errors)))

    out = []
    for num in sorted(assign, key=_pin_order):
        kind, net, shape = assign[num]
        (x, y), (lx, ly), angle = geo[num]
        if kind == 'nc':
            out.append(nc_block(x, y))
            continue
        out.append(wire_block(x, y, lx, ly))
        if kind == 'local':
            out.append(local_label_block(net, lx, ly, angle))
        else:
            out.append(label_block(net, shape, lx, ly, angle))
    generated = ''.join(out)

    n_nc = sum(1 for a in assign.values() if a[0] == 'nc')
    n_local = sum(1 for a in assign.values() if a[0] == 'local')
    print(f"{ref} ({lib_id}) in {sch}: {len(assign)} pins: {len(assign) - n_nc - n_local} global, "
          f"{n_local} local, {n_nc} no-connect; stripped {len(stubs)} stubs, "
          f"{len(remove) - len(stubs)} labels / stub-end no-connects, {len(stale_nc)} pin-end no-connects")
    for num in sorted(assign, key=_pin_order):
        kind, net, shape = assign[num]
        pname = ends[str(num)][3]
        desc = 'NC' if kind == 'nc' else f"{shape if kind == 'global' else 'local':<13} {net}"
        print(f"  {str(num):>3} {pname:<16} {desc}")

    if dry_run:
        print(generated)
        return generated

    spans = sorted(_top_level_span(text, u) for u in remove + stale_nc)
    for (_, e0), (s1, _) in zip(spans, spans[1:]):
        if s1 < e0:
            sys.exit("internal error: overlapping items to strip")
    for s, e in reversed(spans):
        text = text[:s] + text[e:]
    s, _ = _top_level_span(text, atom(find_one(inst, 'uuid')[1]))
    if not text.startswith('\t(symbol\n', s):
        sys.exit("could not locate the symbol instance block in the file text")
    text = text[:s] + generated + text[s:]
    open(sch, 'w').write(text)
    print(f"wrote {sch}")
    return generated


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sch', required=True)
    ap.add_argument('--ref', required=True, help='symbol Reference, e.g. U27')
    ap.add_argument('--pin', action='append', default=[], required=True,
                    help='N=NET:shape (input|output|bidirectional|passive), N=NET:local or N=NC')
    ap.add_argument('--relabel', action='store_true',
                    help='strip previously generated stubs, labels and no-connects on the mentioned pins first')
    ap.add_argument('--dry-run', action='store_true', help='print the generated blocks, write nothing')
    args = ap.parse_args()

    assign = {}
    for spec in args.pin:
        num, a = parse_pin_spec(spec)
        if num in assign:
            sys.exit(f"--pin {spec}: pin {num} given twice")
        assign[num] = a
    apply(args.sch, args.ref, assign, relabel=args.relabel, dry_run=args.dry_run)


if __name__ == '__main__':
    main()
