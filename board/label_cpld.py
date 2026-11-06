#!/usr/bin/env python3
"""Label a CPLD symbol in a KiCad 9 schematic from the //PIN: block of its Verilog.

For every I/O pin of the chosen symbol instance this adds a 5.08 mm wire stub
and a global label named after the Verilog port, with the label shape taken
from the port direction (input / output / inout -> bidirectional).  I/O pins
the Verilog does not assign get a no-connect flag.  Power pins are left alone.
The symbol-side work is relabel_pins.py's.

Verilog -> KiCad net name rules (override or extend with --rename):
  nFOO        -> ~{FOO}            leading lowercase n = active low
  nFOO_k      -> ~{FOO<k+lo>}      active-low vector port nFOO [hi:lo]
  R_nW        -> R{slash}~{W}
  CPUCLK      -> SYSCLK
  BUS_k       -> BUS<k+lo>         vector port [hi:lo]; the PIN block is 0-based
  A rename of a vector port applies to its base (A_hi -> A gives A18..A23).
//PIN: names TDI, TMS, TCK and TDO with no matching port are skipped; their
nets come from --pin (or from cpld_map.py's jtag entry).

With --chip NAME everything comes from cpld_map.py: Verilog, sheet, symbol,
renames, the JTAG nets (pins 23/62 are always JTAG_TMS/JTAG_TCK), fixed pins
(global labels), spare pins (local label <CHIP>_SPARE_<pin>) and leave pins
(untouched).  check_cpld_pins.py checks the result against the netlist.

Examples:
  label_cpld.py --chip ENGINE --relabel
  label_cpld.py --verilog ../cpld/engine/engine.v --sch engine.kicad_sch \
      --ref U18 --pin 14=AUDIO_TO_ENGINE_TD:input --pin 71=ENGINE_TO_PIXEL_TD:output \
      --pin 62=JTAG_TCK:input --pin 23=JTAG_TMS:input

Existing no-connects on the labeled pins are replaced; an existing wire or
label on a pin that would be labeled is an error (nothing is written) unless
--relabel, which first strips the stubs, labels and no-connects a previous
run generated (see relabel_pins.py).
"""

import argparse
import os
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
        if lo is None:
            sys.exit(f"//PIN: {pin_name} indexes scalar port {port}")
        if port in renames:
            return f"{renames[port]}{k + lo}"
        if re.fullmatch(r'n[A-Z][A-Za-z0-9_]*', port):
            return '~{' + port[1:] + str(k + lo) + '}'
        return f"{port}{k + lo}"
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


def lib_symbol_pins(root, lib_id, unit=None):
    """Return [(number, name, type, x, y, rot, hidden)] for a lib symbol.

    With unit, only the pins of that unit (and of the common unit 0)."""
    libs = find_one(root, 'lib_symbols')
    for sym in find_all(libs, 'symbol'):
        if atom(sym[1]) != lib_id:
            continue
        pins = []
        for sub in find_all(sym, 'symbol'):
            m = re.search(r'_(\d+)_(\d+)$', atom(sub[1]))
            if unit is not None and m and int(m.group(1)) not in (0, unit):
                continue
            unit_pins = find_all(sub, 'pin')
            for pin in unit_pins:
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
    """Global label whose connection point is (x, y); angle 0 / 90 / 180 / 270
    points the label body right / up / left / down."""
    left = angle in (180, 270)
    justify = 'right' if left else 'left'
    width = (REF_PAD_LEFT if left else REF_PAD_RIGHT) + CHAR_W * len(name)
    ref_x, ref_y, ref_angle = x, y, 0
    if angle == 0:
        ref_x = x + width
    elif angle == 180:
        ref_x = x - width
    elif angle == 90:
        ref_y, ref_angle = y - width, 90
    elif angle == 270:
        ref_y, ref_angle = y + width, 90
    else:
        raise ValueError(f"label angle {angle}")
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
			(at {fmt(ref_x)} {fmt(ref_y)} {ref_angle})
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


def local_label_block(name, x, y, angle):
    """Local (sheet-scope) label at (x, y); angle as for label_block."""
    justify = 'right' if angle in (180, 270) else 'left'
    return f'''	(label "{name}"
		(at {fmt(x)} {fmt(y)} {angle})
		(effects
			(font
				(size 1.27 1.27)
			)
			(justify {justify} bottom)
		)
		(uuid "{uuid.uuid4()}")
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


# ----------------------------------------------------------------------------
# Chip mode: everything derived from cpld_map.py
# ----------------------------------------------------------------------------

def chip_sheet(chip, board_dir):
    """Return the existing sheet path for a cpld_map chip, or None."""
    for name in (chip['sheet'], chip.get('sheet_fallback')):
        if name and os.path.exists(os.path.join(board_dir, name)):
            return os.path.join(board_dir, name)
    return None


def verilog_assignment(pins, ports, renames, jtag_pins=()):
    """{pin number: (net, shape)} from a //PIN: block.  //PIN: names TDI, TMS,
    TCK and TDO with no matching port are skipped when their pin is in jtag_pins."""
    from cpld_map import JTAG_PIN_NAMES
    assign = {}
    for pin_name, num in pins.items():
        if pin_name in JTAG_PIN_NAMES and pin_name not in ports:
            if num != JTAG_PIN_NAMES[pin_name]:
                sys.exit(f"//PIN: {pin_name} : {num} is not the JTAG {pin_name} pin")
            if num not in jtag_pins:
                sys.exit(f"//PIN: {pin_name} : {num} has no port and no JTAG net")
            continue
        port, _ = pin_to_port(pin_name, ports)
        assign[num] = (net_name(pin_name, ports, renames), SHAPES[ports[port][0]])
    return assign


def chip_assignment(name, board_dir=None):
    """Expected labeling of a cpld_map chip: {pin: (kind, net, shape)} with kind
    'global' | 'local' | 'nc', covering every I/O pin except the chip's leave pins."""
    import cpld_map
    board_dir = board_dir or os.path.dirname(os.path.abspath(__file__))
    if name not in cpld_map.CHIPS:
        sys.exit(f"unknown chip {name}; known: {sorted(cpld_map.CHIPS)}")
    chip = cpld_map.CHIPS[name]
    renames = dict(DEFAULT_RENAMES)
    renames.update(chip['renames'])
    pins, ports = read_verilog(os.path.join(board_dir, chip['verilog']))
    for pin, net in chip['jtag'].items():
        if cpld_map.JTAG_SHAPES.get(pin) is None:
            sys.exit(f"{name}: jtag pin {pin} is not a JTAG pin")

    result = {}

    def put(num, entry, why):
        if num in result:
            sys.exit(f"{name}: pin {num} is both {result[num]} and {entry} ({why})")
        if num not in cpld_map.IO_PINS:
            sys.exit(f"{name}: pin {num} ({why}) is not an I/O pin")
        result[num] = entry

    for num, (net, shape) in verilog_assignment(pins, ports, renames, chip['jtag']).items():
        put(num, ('global', net, shape), 'Verilog')
    for num, net in chip['jtag'].items():
        put(num, ('global', net, cpld_map.JTAG_SHAPES[num]), 'jtag')
    for num, (net, shape) in chip['fixed'].items():
        put(num, ('global', net, shape), 'fixed')
    for num in chip['spare']:
        put(num, ('local', f"{name}_SPARE_{num}", None), 'spare')
    for num in chip['leave']:
        put(num, ('leave', None, None), 'leave')
    for num in cpld_map.IO_PINS:
        result.setdefault(num, ('nc', None, None))
    return {n: a for n, a in result.items() if a[0] != 'leave'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--chip', help='chip name from cpld_map.py (GLUE, PORTS, AUDIO, ENGINE, PIXEL, COMPOSITOR); '
                    'replaces --verilog/--sch/--ref/--pin/--rename')
    ap.add_argument('--verilog')
    ap.add_argument('--sch')
    ap.add_argument('--ref', help='symbol Reference, e.g. U13')
    ap.add_argument('--pin', action='append', default=[],
                    help='extra pin NUM=NET:shape (shape input|output|bidirectional), e.g. JTAG')
    ap.add_argument('--rename', action='append', default=[],
                    help='VERILOG=KICAD net name override (applies to a port or //PIN: name)')
    ap.add_argument('--relabel', action='store_true',
                    help='strip what a previous run generated on the pins before labeling (see relabel_pins.py)')
    ap.add_argument('--board-dir', help='with --chip: directory holding cpld_map sheets (default: this script\'s)')
    ap.add_argument('--dry-run', action='store_true', help='print the generated blocks, write nothing')
    args = ap.parse_args()

    import relabel_pins

    if args.chip:
        if args.verilog or args.sch or args.ref or args.pin or args.rename:
            sys.exit("--chip replaces --verilog/--sch/--ref/--pin/--rename")
        import cpld_map
        board_dir = args.board_dir or os.path.dirname(os.path.abspath(__file__))
        chip = cpld_map.CHIPS.get(args.chip)
        if chip is None:
            sys.exit(f"unknown chip {args.chip}; known: {sorted(cpld_map.CHIPS)}")
        sch = chip_sheet(chip, board_dir)
        if sch is None:
            sys.exit(f"{args.chip}: sheet {chip['sheet']} missing in {board_dir}")
        print(f"{args.chip}: using {sch}")
        assign = chip_assignment(args.chip, board_dir)
        root = parse_sexp(open(sch).read())[0]
        _, lib_id, ends = relabel_pins.symbol_pin_ends(root, chip['ref'])
        io = relabel_pins.io_pin_numbers(ends)
        if io != cpld_map.IO_PINS:
            sys.exit(f"{chip['ref']} ({lib_id}) I/O pins differ from cpld_map.IO_PINS: "
                     f"{sorted(set(io) ^ set(cpld_map.IO_PINS))}")
        relabel_pins.apply(sch, chip['ref'], assign, relabel=args.relabel, dry_run=args.dry_run)
        return

    if not (args.verilog and args.sch and args.ref):
        sys.exit("need --chip, or all of --verilog, --sch and --ref")
    renames = dict(DEFAULT_RENAMES)
    for r in args.rename:
        k, v = r.split('=', 1)
        renames[k] = v

    extra = {}
    for spec in args.pin:
        num, rest = spec.split('=', 1)
        net, shape = rest.rsplit(':', 1)
        if shape not in SHAPES.values():
            sys.exit(f"--pin {spec}: shape must be one of {sorted(SHAPES.values())}")
        extra[int(num)] = (net, shape)

    pins, ports = read_verilog(args.verilog)
    assign = {}
    for num, (net, shape) in verilog_assignment(pins, ports, renames, extra).items():
        assign[num] = ('global', net, shape)
    for num, (net, shape) in extra.items():
        if num in assign:
            sys.exit(f"--pin {num}={net}:{shape}: pin {num} is already {assign[num][1]} from the Verilog")
        assign[num] = ('global', net, shape)

    root = parse_sexp(open(args.sch).read())[0]
    _, lib_id, ends = relabel_pins.symbol_pin_ends(root, args.ref)
    io = relabel_pins.io_pin_numbers(ends)
    unknown = sorted(set(assign) - set(io))
    if unknown:
        sys.exit(f"assigned pins are not I/O pins of {lib_id}: {unknown}")
    for num in io:
        assign.setdefault(num, ('nc', None, None))
    relabel_pins.apply(args.sch, args.ref, assign, relabel=args.relabel, dry_run=args.dry_run)


if __name__ == '__main__':
    main()
