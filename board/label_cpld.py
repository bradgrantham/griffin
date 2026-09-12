#!/usr/bin/env python3
"""Label a CPLD symbol in a KiCad 9 schematic from the //PIN: block of its Verilog.

For every I/O pin of the chosen symbol instance this adds a 5.08 mm wire stub
and a global label named after the Verilog port, with the label shape taken
from the port direction (input / output / inout -> bidirectional).  I/O pins
the Verilog does not assign get a no-connect flag.  Power pins are left alone.

Verilog -> KiCad net name rules (override or extend with --rename):
  nFOO        -> ~{FOO}            leading lowercase n = active low
  R_nW        -> R{slash}~{W}
  CPUCLK      -> SYSCLK
  BUS_k       -> BUS<k+lo>         vector port [hi:lo]; the PIN block is 0-based

Example:
  label_cpld.py --verilog ../cpld/engine/engine.v --sch expansion.kicad_sch \
      --ref U13 --pin 14=TIMING_TO_ENGINE_TD:input --pin 71=ENGINE_TO_PIXEL_TD:output \
      --pin 62=JTAG_TCK:input --pin 23=JTAG_TMS:input

Existing no-connects on the symbol's pins are replaced; an existing wire or
label on a pin that would be labeled is an error (nothing is written).
"""

import argparse
import re
import sys
import uuid

# ----------------------------------------------------------------------------
# Minimal s-expression reader (enough for KiCad schematic files)
# ----------------------------------------------------------------------------

_TOKEN = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))', re.S)


def parse_sexp(text):
    pos = 0
    stack = [[]]
    while text[pos:].strip():
        m = _TOKEN.match(text, pos)
        if not m:
            raise ValueError(f"bad s-expression near offset {pos}")
        pos = m.end()
        if m.group(1):
            stack.append([])
        elif m.group(2):
            done = stack.pop()
            stack[-1].append(done)
        elif m.group(3) is not None:
            stack[-1].append(('str', m.group(3)))
        elif m.group(4) is not None:
            stack[-1].append(m.group(4))
        else:
            break
    if len(stack) != 1:
        raise ValueError("unbalanced parentheses")
    return stack[0]


def atom(x):
    """Return the python value of an atom (string or bare token)."""
    return x[1] if isinstance(x, tuple) else x


def find_all(node, head):
    return [n for n in node if isinstance(n, list) and n and n[0] == head]


def find_one(node, head):
    found = find_all(node, head)
    return found[0] if found else None


# ----------------------------------------------------------------------------
# Verilog side
# ----------------------------------------------------------------------------

def read_verilog(path):
    """Return ({pin_name: pin_number}, {port_name: (direction, lo)})."""
    text = open(path).read()
    pins = {}
    for m in re.finditer(r'^\s*//PIN:\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(\d+)\s*$', text, re.M):
        name, num = m.group(1), int(m.group(2))
        if name in pins:
            sys.exit(f"{path}: pin name {name} assigned twice")
        if num in pins.values():
            sys.exit(f"{path}: pin {num} assigned twice")
        pins[name] = num

    ports = {}
    port_re = re.compile(
        r'^\s*(input|output|inout)\s+(?:wire|reg)?\s*(?:\[\s*(\d+)\s*:\s*(\d+)\s*\])?\s*'
        r'([A-Za-z_][A-Za-z0-9_]*)\s*[,)]?', re.M)
    for m in port_re.finditer(text):
        direction, hi, lo, name = m.groups()
        lo = int(lo) if lo is not None else None
        ports[name] = (direction, lo)
    if not pins or not ports:
        sys.exit(f"{path}: found {len(pins)} //PIN: lines and {len(ports)} ports")
    return pins, ports


DEFAULT_RENAMES = {
    'R_nW': 'R{slash}~{W}',
    'CPUCLK': 'SYSCLK',
}


def pin_to_port(pin_name, ports):
    """Map a //PIN: name to (port_name, bit index or None)."""
    if pin_name in ports:
        return pin_name, None
    m = re.fullmatch(r'(.+)_(\d+)', pin_name)
    if m and m.group(1) in ports:
        return m.group(1), int(m.group(2))
    sys.exit(f"//PIN: {pin_name} has no matching port")


def net_name(pin_name, ports, renames):
    port, k = pin_to_port(pin_name, ports)
    direction, lo = ports[port]
    if pin_name in renames:
        return renames[pin_name]
    if k is not None:
        base = renames.get(port, port)
        if lo is None:
            sys.exit(f"//PIN: {pin_name} indexes scalar port {port}")
        return f"{base}{k + lo}"
    if re.fullmatch(r'n[A-Z][A-Za-z0-9_]*', port):
        return '~{' + port[1:] + '}'
    return port


SHAPES = {'input': 'input', 'output': 'output', 'inout': 'bidirectional'}


# ----------------------------------------------------------------------------
# Schematic side
# ----------------------------------------------------------------------------

def symbol_instance(root, ref):
    for sym in find_all(root, 'symbol'):
        for prop in find_all(sym, 'property'):
            if atom(prop[1]) == 'Reference' and atom(prop[2]) == ref:
                return sym
    sys.exit(f"no symbol with Reference {ref}")


def lib_symbol_pins(root, lib_id):
    """Return [(number, name, type, x, y, rot, hidden)] for a lib symbol."""
    libs = find_one(root, 'lib_symbols')
    for sym in find_all(libs, 'symbol'):
        if atom(sym[1]) != lib_id:
            continue
        pins = []
        for unit in find_all(sym, 'symbol'):
            for pin in find_all(unit, 'pin'):
                at = find_one(pin, 'at')
                name = atom(find_one(pin, 'name')[1])
                number = atom(find_one(pin, 'number')[1])
                hidden = find_one(pin, 'hide') is not None
                pins.append((number, name, atom(pin[1]),
                             float(atom(at[1])), float(atom(at[2])), int(float(atom(at[3]))), hidden))
        return pins
    sys.exit(f"lib symbol {lib_id} not found")


def fmt(v):
    s = f"{v:.4f}".rstrip('0').rstrip('.')
    return s if s not in ('-0', '') else '0'


STUB = 5.08
CHAR_W = 0.806          # Intersheetrefs offset model fitted from existing labels
REF_PAD_RIGHT = 4.96
REF_PAD_LEFT = 3.67


def label_block(name, shape, x, y, angle):
    left = angle == 180
    justify = 'right' if left else 'left'
    ref_x = x - (REF_PAD_LEFT + CHAR_W * len(name)) if left else x + (REF_PAD_RIGHT + CHAR_W * len(name))
    return f'''	(global_label "{name}"
		(shape {shape})
		(at {fmt(x)} {fmt(y)} {angle})
		(fields_autoplaced yes)
		(effects
			(font
				(size 1.27 1.27)
			)
			(justify {justify})
		)
		(uuid "{uuid.uuid4()}")
		(property "Intersheetrefs" "${{INTERSHEET_REFS}}"
			(at {fmt(ref_x)} {fmt(y)} 0)
			(effects
				(font
					(size 1.27 1.27)
				)
				(justify {justify})
				(hide yes)
			)
		)
	)
'''


def wire_block(x1, y1, x2, y2):
    return f'''	(wire
		(pts
			(xy {fmt(x1)} {fmt(y1)}) (xy {fmt(x2)} {fmt(y2)})
		)
		(stroke
			(width 0)
			(type default)
		)
		(uuid "{uuid.uuid4()}")
	)
'''


def nc_block(x, y):
    return f'''	(no_connect
		(at {fmt(x)} {fmt(y)})
		(uuid "{uuid.uuid4()}")
	)
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--verilog', required=True)
    ap.add_argument('--sch', required=True)
    ap.add_argument('--ref', required=True, help='symbol Reference, e.g. U13')
    ap.add_argument('--pin', action='append', default=[],
                    help='extra pin NUM=NET:shape (shape input|output|bidirectional), e.g. JTAG')
    ap.add_argument('--rename', action='append', default=[],
                    help='VERILOG=KICAD net name override (applies to a port or //PIN: name)')
    ap.add_argument('--dry-run', action='store_true', help='print the generated blocks, write nothing')
    args = ap.parse_args()

    renames = dict(DEFAULT_RENAMES)
    for r in args.rename:
        k, v = r.split('=', 1)
        renames[k] = v

    pins, ports = read_verilog(args.verilog)

    # pin number -> (net, shape)
    assign = {}
    for pin_name, num in pins.items():
        port, _ = pin_to_port(pin_name, ports)
        assign[num] = (net_name(pin_name, ports, renames), SHAPES[ports[port][0]])
    for spec in args.pin:
        num, rest = spec.split('=', 1)
        net, shape = rest.rsplit(':', 1)
        if shape not in SHAPES.values():
            sys.exit(f"--pin {spec}: shape must be one of {sorted(SHAPES.values())}")
        num = int(num)
        if num in assign:
            sys.exit(f"--pin {spec}: pin {num} is already {assign[num]} from the Verilog")
        assign[num] = (net, shape)

    text = open(args.sch).read()
    root = parse_sexp(text)[0]
    inst = symbol_instance(root, args.ref)
    lib_id = atom(find_one(inst, 'lib_id')[1])
    at = find_one(inst, 'at')
    sx, sy, srot = float(atom(at[1])), float(atom(at[2])), int(float(atom(at[3])))
    if srot != 0 or find_one(inst, 'mirror') is not None:
        sys.exit(f"{args.ref} is rotated or mirrored; only rotation 0 is supported")

    lib_pins = lib_symbol_pins(root, lib_id)
    io_pins = [(n, nm, t, x, y, r) for (n, nm, t, x, y, r, hidden) in lib_pins
               if not hidden and t not in ('power_in', 'power_out', 'passive', 'no_connect')]

    # Schematic positions of every I/O pin end
    ends = {}
    for number, name, ptype, lx, ly, rot in io_pins:
        ends[int(number)] = (sx + lx, sy - ly, rot, name)

    unknown = sorted(set(assign) - set(ends))
    if unknown:
        sys.exit(f"assigned pins are not I/O pins of {lib_id}: {unknown}")

    # Existing objects touching the pin ends
    def at_xy(node):
        a = find_one(node, 'at')
        return (float(atom(a[1])), float(atom(a[2])))

    end_set = {(fmt(x), fmt(y)) for (x, y, _, _) in ends.values()}
    stale_nc = []
    for nc in find_all(root, 'no_connect'):
        x, y = at_xy(nc)
        if (fmt(x), fmt(y)) in end_set:
            stale_nc.append(atom(find_one(nc, 'uuid')[1]))
    clashes = []
    for w in find_all(root, 'wire'):
        for xy in find_all(find_one(w, 'pts'), 'xy'):
            if (fmt(float(atom(xy[1]))), fmt(float(atom(xy[2])))) in end_set:
                clashes.append(('wire', xy))
    for head in ('global_label', 'label', 'hierarchical_label'):
        for lb in find_all(root, head):
            x, y = at_xy(lb)
            if (fmt(x), fmt(y)) in end_set:
                clashes.append((head, atom(lb[1])))
    if clashes:
        sys.exit(f"existing objects on {args.ref} pin ends, refusing: {clashes}")

    out = []
    nc_pins = []
    for num in sorted(ends):
        x, y, rot, pname = ends[num]
        if num in assign:
            net, shape = assign[num]
            if rot == 0:        # pin points right: end is on the left of the body
                lx, angle = x - STUB, 180
            elif rot == 180:
                lx, angle = x + STUB, 0
            else:
                sys.exit(f"pin {num} rotation {rot} not supported")
            out.append(wire_block(x, y, lx, y))
            out.append(label_block(net, shape, lx, y, angle))
        else:
            nc_pins.append((num, pname))
            out.append(nc_block(x, y))
    generated = ''.join(out)

    print(f"{args.ref} ({lib_id}): {len(ends)} I/O pins, {len(assign)} labeled, "
          f"{len(nc_pins)} no-connect, {len(stale_nc)} existing no-connect removed")
    for num, pname in nc_pins:
        print(f"  NC pin {num} ({pname})")
    for num in sorted(assign):
        print(f"  {num:>3} {assign[num][1]:<13} {assign[num][0]}")

    if args.dry_run:
        print(generated)
        return

    # Text surgery: drop stale no-connect blocks, insert before the instance block.
    for u in stale_nc:
        text, n = re.subn(r'\t\(no_connect\n\t\t\(at [^\n]*\n\t\t\(uuid "' + re.escape(u) + r'"\)\n\t\)\n', '', text)
        if n != 1:
            sys.exit(f"could not remove no_connect {u}")
    inst_uuid = atom(find_one(inst, 'uuid')[1])
    m = re.search(r'\t\(symbol\n\t\t\(lib_id "' + re.escape(lib_id) + r'"\)\n(?:[^\n]*\n)*?\t\t\(uuid "' + re.escape(inst_uuid) + r'"\)', text)
    if not m or '\t(symbol\n' in text[m.start() + 1:m.end()]:
        sys.exit("could not locate the symbol instance block in the file text")
    text = text[:m.start()] + generated + text[m.start():]
    open(args.sch, 'w').write(text)
    print(f"wrote {args.sch}")


if __name__ == '__main__':
    main()
