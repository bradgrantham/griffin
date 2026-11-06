#!/usr/bin/env python3
"""Place IDT7200 FIFOs, with bypass caps, /R series resistors, empty-flag test
points and global labels, on a KiCad 9 sheet.

Each group is described by an entry in PAIRS below (chip references, sheet
positions, data-bus label base and byte, strobes, Q bus, capacitor, resistor
and test-point references).  A group either gangs its chips behind one shared
read strobe (shared_re: one series resistor to a local net on the FIFO side)
or gives every chip its own write strobe, read strobe and empty flag.
Pin handling per lib pin name:
  D0..D7 -> <d_base><d_lo+i> (input)  D8 -> GND
  Q0..Q7 -> Q bus label (tri_state)   Q8 -> no-connect
  ~{W}   -> write strobe label        ~{RS} -> reset label (per group)
  ~{R}   -> series resistor, then the read strobe label on the driver side
            (or the shared local net when the group has shared_re)
  ~{EF}  -> empty-flag label (output), plus a Connector:TestPoint on the same
            stub end when the chip names one; otherwise no-connect
  ~{XI}  -> GND (single device mode) ~{FL}/~{RT} -> +5V (retransmit held high)
  ~{FF}, ~{HF}/~{XO} -> no-connect
  VCC -> +5V, GND -> GND power symbols on the pin ends

Uses the KiCad Memory_RAM:IDT7201 symbol (identical DIP-28 pinout to the
7200) and Device:R / Device:C / Connector:TestPoint / power:+5V / power:GND.
Lib symbols are copied into the sheet's lib_symbols when absent; a nickname in
the sheet directory's sym-lib-table is resolved before KiCad's own libraries,
and derived ("extends") symbols are flattened the way eeschema stores them.

Chip references and power references must be free on every sheet in the
directory (refused otherwise).  A resistor, capacitor or test-point reference
that is taken is replaced by the next free number, and the substitution is
printed.  Power references are the first free #PWR numbers from pwr_start.

Example:  place_fifo.py --sch audio-fifo.kicad_sch --pair audio_samples
"""

import argparse
import glob
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from label_cpld import fmt, wire_block, label_block, nc_block, parse_sexp, find_all, find_one, atom  # noqa: E402

KICAD_SYMS = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols'
STUB = 5.08

# KiCad's IDT7201 symbol (the one with tri_state Q pins, needed for the shared
# Q bus) names its control pins without overbars; normalise to the datasheet names.
NAME_ALIASES = {'WR': '~{W}', 'RD': '~{R}', 'CLR': '~{RS}', 'XI': '~{XI}', 'FL': '~{FL}/~{RT}',
                'XO': '~{HF}/~{XO}', 'EF': '~{EF}', 'FF': '~{FF}', 'VSS': 'GND'}

# Per group: d_base (data-input label base), rs (reset net), series (read-strobe
# resistor value), pwr_start; w / q may be per group or per chip.
# Per chip: ref, at, d_lo, q_lo (default 0), cap, cap_at, and either the
# group's shared_re or its own re + res; optional ef (+ tp) for the empty flag.
PAIRS = {
    # Ganged 16-bit pair: HIGH holds D[15:8] -> Q8..15, LOW holds D[7:0] -> Q0..7,
    # one read strobe through one series resistor to both /R pins (local net on
    # the FIFO side), no empty flag.
    'pixels': dict(
        w='~{PIXELS_FIFO_W}', rs='~{RS}', q='PIXELS_Q', d_base='D_VIDEO_FIFO', series='0R', pwr_start=144,
        shared_re=dict(net='~{PIXELS_RE}', res='R28', local='~{PIXELS_RE_FIFO_SIDE}', at=(233.68, 92.71)),
        chips=[
            dict(ref='U20', at=(210.82, 55.88), d_lo=8, q_lo=8, cap='C27', cap_at=(177.8, 78.74)),
            dict(ref='U21', at=(210.82, 129.54), d_lo=0, q_lo=0, cap='C28', cap_at=(177.8, 152.4)),
        ]),
    # Same ganging; empty flag taken from the LOW chip.
    'vidcmd': dict(
        w='~{VIDCMD_FIFO_W}', rs='~{RS}', q='VIDCMD_Q', d_base='D_VIDEO_FIFO', series='0R', pwr_start=162,
        shared_re=dict(net='~{VIDCMD_RE}', res='R30', local='~{VIDCMD_RE_FIFO_SIDE}', at=(233.68, 92.71)),
        chips=[
            dict(ref='U23', at=(210.82, 55.88), d_lo=8, q_lo=8, cap='C30', cap_at=(177.8, 78.74)),
            dict(ref='U24', at=(210.82, 129.54), d_lo=0, q_lo=0, ef='~{VIDCMD_EF}', cap='C31', cap_at=(177.8, 152.4)),
        ]),
    # Four independent sample FIFOs, one per AUDIO channel: ch0/ch1 on
    # D_AUDIO_FIFO[15:8], ch2/ch3 on D_AUDIO_FIFO[7:0].  Two columns of two.
    'audio_samples': dict(
        rs='~{RESET}', d_base='D_AUDIO_FIFO', series='0R', pwr_start=176,
        chips=[
            dict(ref=f'U{u}', at=at, d_lo=d_lo, w=f'~{{SAMPLES{n}_FIFO_W}}', re=f'~{{SAMPLE_R{n}}}', res=f'R{34 + n}',
                 ef=f'~{{SAMPLE_EF{n}}}', tp=f'TP{3 + n}', q=f'SAMPLE{n}_Q', cap=f'C{36 + n}',
                 cap_at=(at[0] - 33.02, at[1] + 22.86))
            for n, (u, at, d_lo) in enumerate([(25, (101.6, 55.88), 8), (26, (101.6, 129.54), 8),
                                               (30, (215.9, 55.88), 0), (31, (215.9, 129.54), 0)])
        ]),
    # AUDIO control FIFO: right column, top, so it can share a sheet with a CPLD.
    'audctl': dict(
        rs='~{RESET}', d_base='D_AUDIO_FIFO', series='0R', pwr_start=218,
        chips=[
            dict(ref='U29', at=(210.82, 55.88), d_lo=0, w='~{AUDCTL_FIFO_W}', re='~{CONTROL_R}', res='R33',
                 ef='~{CONTROL_EF}', tp='TP7', q='CONTROL_Q', cap='C40', cap_at=(177.8, 78.74)),
        ]),
}

FIFO = dict(lib='Memory_RAM', name='IDT7201', value='IDT7200L15P',
            footprint='Package_DIP:DIP-28_W7.62mm',
            datasheet='https://www.renesas.com/us/en/document/dst/7200-7202-datasheet',
            description='256x9 CMOS asynchronous FIFO, DIP-28 300 mil')
RES = dict(lib='Device', name='R', footprint='Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal',
           description='Resistor')
CAP = dict(lib='Device', name='C', value='100nF', footprint='Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm',
           description='Unpolarized capacitor')
TESTPOINT = dict(lib='Connector', name='TestPoint', value='TestPoint', footprint='TestPoint:TestPoint_Pad_D1.5mm',
                 description='test point')


# ----------------------------------------------------------------------------
# s-expression text helpers
# ----------------------------------------------------------------------------

def block_span(text, start_pat, pos=0):
    """(start, end) of the balanced block whose opening matches start_pat."""
    m = re.compile(start_pat, re.M).search(text, pos)
    if not m:
        return None
    i = m.start()
    depth = 0
    instr = False
    j = i
    while j < len(text):
        c = text[j]
        if instr:
            if c == '\\':
                j += 1
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i, j + 1
        j += 1
    sys.exit("unbalanced s-expression")


def library_path(libname, project_dir=None):
    """File for a library nickname: the project's sym-lib-table first, then KiCad's own libraries."""
    if project_dir:
        table = os.path.join(project_dir, 'sym-lib-table')
        if os.path.exists(table):
            m = re.search(r'\(lib \(name "?' + re.escape(libname) + r'"?\)\s*\(type "?[^)"]*"?\)\s*\(uri "([^"]+)"\)',
                          open(table).read())
            if m:
                return m.group(1).replace('${KIPRJMOD}', project_dir)
    return os.path.join(KICAD_SYMS, libname + '.kicad_sym')


def top_symbol(text, name, path):
    """Text of the library's top-level `(symbol "NAME" ...)` block (unit blocks are NAME_u_s)."""
    span = block_span(text, r'\(symbol "' + re.escape(name) + r'"(?=[\s)])')
    if not span:
        sys.exit(f"{name} not found in {path}")
    return text[span[0]:span[1]]


def flatten(text, blk, name, path):
    """Resolve `(extends "PARENT")`: the parent's body under the child's name and properties."""
    m = re.search(r'\(extends "([^"]+)"\)', blk)
    if not m:
        return blk
    parent = m.group(1)
    out = top_symbol(text, parent, path)
    out = out.replace(f'(symbol "{parent}"', f'(symbol "{name}"', 1)
    out = out.replace(f'(symbol "{parent}_', f'(symbol "{name}_')
    for pm in re.finditer(r'\(property "([^"]+)"', blk):
        ps = block_span(blk, r'\(property "' + re.escape(pm.group(1)) + '"')
        child_prop = blk[ps[0]:ps[1]]
        span = block_span(out, r'\(property "' + re.escape(pm.group(1)) + '"')
        if span:
            out = out[:span[0]] + child_prop + out[span[1]:]
        else:                   # child-only field: insert before the first unit block
            u = re.search(r'\(symbol "' + re.escape(name) + '_', out)
            indent = re.search(r'\n([ \t]*)\(symbol "' + re.escape(name) + '_', out).group(1)
            out = out[:u.start()] + child_prop + '\n' + indent + out[u.start():]
    return out


def lib_symbol_block(libname, name, project_dir=None):
    """The `(symbol "NAME" ...)` block from a library, flattened, re-keyed and re-indented for a sheet."""
    path = library_path(libname, project_dir)
    text = open(path).read()
    blk = flatten(text, top_symbol(text, name, path), name, path)
    blk = blk.replace(f'(symbol "{name}"', f'(symbol "{libname}:{name}"', 1)
    lines = blk.split('\n')
    return '\t\t' + lines[0] + '\n' + '\n'.join('\t' + l if l else l for l in lines[1:]) + '\n'


def ensure_lib_symbol(sheet, libname, name, project_dir=None):
    lib_id = f'{libname}:{name}'
    if f'(symbol "{lib_id}"' in sheet:
        return sheet
    s, e = block_span(sheet, r'^\t\(lib_symbols')
    blk = lib_symbol_block(libname, name, project_dir)
    if sheet[s:e].strip() == '(lib_symbols)':   # empty sheet
        return sheet[:s] + '\t(lib_symbols\n' + blk + '\t)' + sheet[e:]
    # insert before the closing paren of lib_symbols (which sits on its own line "\t)")
    close = sheet.rfind('\n\t)', s, e)
    return sheet[:close + 1] + blk.rstrip('\n') + sheet[close:]


def lib_pins(sheet, lib_id, unit=None, full=False):
    """[(number, name, lx, ly, rot)] of a lib symbol already present in the sheet.

    unit=None returns every pin of every unit (single-unit symbols); unit=n returns
    unit n's pins plus the common unit 0, body style 1 only.  full=True appends
    (type, length, hidden) to each tuple."""
    root = parse_sexp(sheet)[0]
    for sym in find_all(find_one(root, 'lib_symbols'), 'symbol'):
        if atom(sym[1]) != lib_id:
            continue
        pins = []
        for sub in find_all(sym, 'symbol'):
            m = re.search(r'_(\d+)_(\d+)$', atom(sub[1]))
            if unit is not None and m and (int(m.group(1)) not in (0, unit) or int(m.group(2)) not in (0, 1)):
                continue
            for pin in find_all(sub, 'pin'):
                at = find_one(pin, 'at')
                p = (atom(find_one(pin, 'number')[1]), atom(find_one(pin, 'name')[1]),
                     float(atom(at[1])), float(atom(at[2])), int(float(atom(at[3]))))
                if full:
                    length = find_one(pin, 'length')
                    hide = find_one(pin, 'hide')
                    hidden = 'hide' in pin or (hide is not None and (len(hide) == 1 or atom(hide[1]) == 'yes'))
                    p += (atom(pin[1]), float(atom(length[1])) if length else 2.54, hidden)
                pins.append(p)
        return pins
    sys.exit(f"{lib_id} missing from lib_symbols")


def free_refs(wanted, used, kind):
    """Map each wanted reference to itself, or to the next free number with its prefix when taken."""
    taken = set(used)
    out = {}
    for ref in wanted:
        new = ref
        if new in taken:
            prefix, n = re.fullmatch(r'(\D+)(\d+)', ref).groups()
            n = int(n)
            while f'{prefix}{n}' in taken:
                n += 1
            new = f'{prefix}{n}'
            print(f"{kind} {ref} is taken; using {new}")
        taken.add(new)
        out[ref] = new
    return out


def pwr_refs_from(start, count, used):
    """The first `count` free #PWR references numbered from `start`."""
    refs, n = [], start
    while len(refs) < count:
        r = f'#PWR{n:04d}'
        if r not in used:
            refs.append(r)
        n += 1
    return refs


def used_references(sch):
    """Every Reference in the .kicad_sch files beside sch."""
    used = set()
    for f in glob.glob(os.path.join(os.path.dirname(os.path.abspath(sch)), '*.kicad_sch')):
        used |= set(re.findall(r'\(reference "([^"]+)"', open(f).read()))
    return used


def sheet_path(sch):
    """Instance path of a sheet: from its symbols, or from the root's (sheet ...) block when empty."""
    sheet = open(sch).read()
    paths = set(re.findall(r'\(path "(/[0-9a-f-]+/[0-9a-f-]+)"', sheet))
    if len(paths) == 1:
        return paths.pop()
    if paths:
        sys.exit(f"expected one instance path in {sch}, found {paths}")
    root_file = os.path.join(os.path.dirname(os.path.abspath(sch)), 'board.kicad_sch')
    rtext = open(root_file).read()
    root_uuid = re.search(r'^\t\(uuid "([0-9a-f-]+)"\)', rtext, re.M).group(1)
    m = re.search(r'\(uuid "([0-9a-f-]+)"\)\n\t\t\(property "Sheetname" "[^"]*"\n(?:\t\t[^\n]*\n)*?\t\t\(property "Sheetfile" "'
                  + re.escape(os.path.basename(sch)) + '"', rtext)
    if not m:
        sys.exit(f"{root_file} has no sheet block for {os.path.basename(sch)}")
    return f'/{root_uuid}/{m.group(1)}'


# ----------------------------------------------------------------------------
# block emitters
# ----------------------------------------------------------------------------

def prop(name, value, x, y, hide=False, justify=None, angle=0):
    # Field text is rotated with the symbol, so a rotated symbol's visible
    # fields get a compensating angle to stay horizontal.
    eff = '\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n'
    if justify:
        eff += f'\t\t\t\t(justify {justify})\n'
    if hide:
        eff += '\t\t\t\t(hide yes)\n'
    value = value.replace('"', '\\"')
    return f'\t\t(property "{name}" "{value}"\n\t\t\t(at {fmt(x)} {fmt(y)} {angle})\n\t\t\t(effects\n{eff}\t\t\t)\n\t\t)\n'


def instance_block(lib_id, x, y, rot, ref, props, pin_numbers, path, unit=1):
    out = f'\t(symbol\n\t\t(lib_id "{lib_id}")\n\t\t(at {fmt(x)} {fmt(y)} {rot})\n\t\t(unit {unit})\n'
    out += '\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)\n\t\t(fields_autoplaced yes)\n'
    out += f'\t\t(uuid "{uuid.uuid4()}")\n'
    out += ''.join(props)
    for n in pin_numbers:
        out += f'\t\t(pin "{n}"\n\t\t\t(uuid "{uuid.uuid4()}")\n\t\t)\n'
    out += f'\t\t(instances\n\t\t\t(project "board"\n\t\t\t\t(path "{path}"\n\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit {unit})\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n'
    return out


def local_label_block(name, x, y, angle):
    justify = 'right bottom' if angle in (180, 270) else 'left bottom'
    return f'''	(label "{name}"
		(at {fmt(x)} {fmt(y)} {angle})
		(fields_autoplaced yes)
		(effects
			(font
				(size 1.27 1.27)
			)
			(justify {justify})
		)
		(uuid "{uuid.uuid4()}")
	)
'''


def power_block(kind, x, y, rot, ref, path):
    """kind '+5V' or 'GND'; glyph hangs off the connection point (x, y)."""
    up = kind == '+5V'
    # Reference hidden; Value placed past the glyph tip, on the axis the glyph points along.
    if rot == 0:
        vx, vy, va, vj = x, y + (-5.08 if up else 5.08), 0, None
    else:                       # glyph lies along the row; value past its tip, kept horizontal
        vx, vy, va, vj = x + (5.08 if up else -5.08), y, (360 - rot) % 180, ('left' if up else 'right')
    props = [
        prop('Reference', ref, x, y + (3.81 if up else 6.35), hide=True),
        prop('Value', kind, vx, vy, justify=vj, angle=va),
        prop('Footprint', '', x, y, hide=True),
        prop('Datasheet', '', x, y, hide=True),
        prop('Description', f'Power symbol creates a global label with name "{kind}"' + ('' if up else ' , ground'), x, y, hide=True),
    ]
    return instance_block(f'power:{kind}', x, y, rot, ref, props, ['1'], path)


def two_pin_block(part, x, y, rot, ref, value, path):
    if rot == 0:                # vertical part: fields to its right
        props = [prop('Reference', ref, x + 3.81, y - 1.27, justify='left'),
                 prop('Value', value, x + 3.81, y + 1.27, justify='left')]
    else:                       # horizontal part: fields above and below, kept horizontal
        a = (360 - rot) % 180
        props = [prop('Reference', ref, x, y - 4.699, angle=a),
                 prop('Value', value, x, y - 2.286, angle=a)]
    props += [
        prop('Footprint', part['footprint'], x, y, hide=True),
        prop('Datasheet', '~', x, y, hide=True),
        prop('Description', part['description'], x, y, hide=True),
    ]
    return instance_block(f"{part['lib']}:{part['name']}", x, y, rot, ref, props, ['1', '2'], path)


def testpoint_block(x, y, vertical_stub, ref, path):
    """Connector:TestPoint whose pin sits on (x, y).  On a horizontal stub the pad
    stands up; on a vertical stub it lies to the left.  Reference beside the pad."""
    if not vertical_stub:
        rot, a = 0, 0
        props = [prop('Reference', ref, x + 1.524, y - 3.302, justify='left')]
    else:
        rot, a = 90, 90
        props = [prop('Reference', ref, x - 3.302, y - 2.032, angle=a)]
    props += [
        prop('Value', TESTPOINT['value'], x, y, hide=True, angle=a),
        prop('Footprint', TESTPOINT['footprint'], x, y, hide=True),
        prop('Datasheet', '~', x, y, hide=True),
        prop('Description', TESTPOINT['description'], x, y, hide=True),
    ]
    return instance_block(f"{TESTPOINT['lib']}:{TESTPOINT['name']}", x, y, rot, ref, props, ['1'], path)


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sch', required=True)
    ap.add_argument('--pair', required=True, choices=sorted(PAIRS))
    args = ap.parse_args()
    cfg = PAIRS[args.pair]
    chips = cfg['chips']

    sheet = open(args.sch).read()
    path = sheet_path(args.sch)

    # reference clash check across the project
    used = used_references(args.sch)
    clash = sorted({c['ref'] for c in chips} & used)
    if clash:
        sys.exit(f"references already in use: {clash}")
    passives = [c['cap'] for c in chips] + [c['res'] for c in chips if 'res' in c] + [c['tp'] for c in chips if 'tp' in c]
    if 'shared_re' in cfg:
        passives.append(cfg['shared_re']['res'])
    ref = free_refs(passives, used | {c['ref'] for c in chips}, 'reference')
    # per chip: VCC, GND, /XI, D8, /FL ties, plus the bypass cluster's +5V and GND
    pwr_refs = pwr_refs_from(cfg['pwr_start'], 7 * len(chips), used)

    for part in (FIFO, RES, CAP) + ((TESTPOINT,) if any('tp' in c for c in chips) else ()):
        sheet = ensure_lib_symbol(sheet, part['lib'], part['name'])
    for kind in ('+5V', 'GND'):
        sheet = ensure_lib_symbol(sheet, 'power', kind)

    fifo_id = f"{FIFO['lib']}:{FIFO['name']}"
    pins = lib_pins(sheet, fifo_id)
    pin_numbers = [p[0] for p in pins]

    out = []
    pwr = iter(pwr_refs)
    for chip in chips:
        sx, sy = chip['at']
        props = [
            prop('Reference', chip['ref'], sx - 2.54, sy - 23.495, justify='right'),
            prop('Value', FIFO['value'], sx - 2.54, sy - 20.955, justify='right'),
            prop('Footprint', FIFO['footprint'], sx, sy, hide=True),
            prop('Datasheet', FIFO['datasheet'], sx, sy, hide=True),
            prop('Description', FIFO['description'], sx, sy, hide=True),
        ]
        out.append(instance_block(fifo_id, sx, sy, 0, chip['ref'], props, pin_numbers, path))
        w = chip.get('w', cfg.get('w'))
        q = chip.get('q', cfg.get('q'))

        for number, name, lx, ly, rot in pins:
            name = NAME_ALIASES.get(name, name)
            px, py = sx + lx, sy - ly
            if rot == 0:        # left side, stub goes -x
                ex, angle, away = px - STUB, 180, -1
            elif rot == 180:
                ex, angle, away = px + STUB, 0, +1
            else:
                ex, angle, away = None, None, 0

            m = re.fullmatch(r'D(\d)', name)
            if m and int(m.group(1)) < 8:
                out += [wire_block(px, py, ex, py), label_block(f"{cfg['d_base']}{chip['d_lo'] + int(m.group(1))}", 'input', ex, py, angle)]
                continue
            m = re.fullmatch(r'Q(\d)', name)
            if m and int(m.group(1)) < 8:
                out += [wire_block(px, py, ex, py), label_block(f"{q}{chip.get('q_lo', 0) + int(m.group(1))}", 'tri_state', ex, py, angle)]
                continue
            if name == '~{W}':
                out += [wire_block(px, py, ex, py), label_block(w, 'input', ex, py, angle)]
            elif name == '~{RS}':
                out += [wire_block(px, py, ex, py), label_block(cfg['rs'], 'input', ex, py, angle)]
            elif name == '~{R}' and 're' not in chip:
                out += [wire_block(px, py, ex, py), local_label_block(cfg['shared_re']['local'], ex, py, angle)]
            elif name == '~{EF}' and 'ef' in chip:
                out += [wire_block(px, py, ex, py), label_block(chip['ef'], 'output', ex, py, angle)]
                if 'tp' in chip:
                    out.append(testpoint_block(ex, py, False, ref[chip['tp']], path))
            elif name == '~{R}':
                # pin -> 2.54 wire -> resistor (7.62 long, horizontal) -> 2.54 wire -> label
                x1 = px + away * 2.54
                rc = x1 + away * 3.81
                x2 = x1 + away * 7.62
                x3 = x2 + away * 2.54
                out += [wire_block(px, py, x1, py),
                        two_pin_block(RES, rc, py, 90, ref[chip['res']], cfg['series'], path),
                        wire_block(x2, py, x3, py),
                        label_block(chip['re'], 'input', x3, py, angle)]
            elif name in ('D8', '~{XI}'):
                out += [wire_block(px, py, ex, py), power_block('GND', ex, py, 270, next(pwr), path)]
            elif name == '~{FL}/~{RT}':
                out += [wire_block(px, py, ex, py), power_block('+5V', ex, py, 270, next(pwr), path)]
            elif name in ('Q8', '~{EF}', '~{FF}', '~{HF}/~{XO}'):
                out.append(nc_block(px, py))
            elif name == 'VCC':
                out.append(power_block('+5V', px, py, 0, next(pwr), path))
            elif name == 'GND':
                out.append(power_block('GND', px, py, 0, next(pwr), path))
            else:
                sys.exit(f"unhandled pin {number} {name}")

        # bypass cluster: +5V / C / GND stacked, power symbols on the cap's pin ends
        cx, cy = chip['cap_at']
        out += [two_pin_block(CAP, cx, cy, 0, ref[chip['cap']], CAP['value'], path),
                power_block('+5V', cx, cy - 3.81, 0, next(pwr), path),
                power_block('GND', cx, cy + 3.81, 0, next(pwr), path)]

    if 'shared_re' in cfg:
        # driver-side global label -> series resistor -> FIFO-side local net
        sr = cfg['shared_re']
        x0, y0 = sr['at']
        out += [label_block(sr['net'], 'input', x0, y0, 180),
                wire_block(x0, y0, x0 + 2.54, y0),
                two_pin_block(RES, x0 + 2.54 + 3.81, y0, 90, ref[sr['res']], cfg['series'], path),
                wire_block(x0 + 2.54 + 7.62, y0, x0 + 2.54 + 7.62 + 2.54, y0),
                local_label_block(sr['local'], x0 + 2.54 + 7.62 + 2.54, y0, 0)]

    if next(pwr, None) is not None:
        sys.exit("power reference count mismatch")
    generated = ''.join(out)
    i = sheet.rstrip().rfind(')')
    sheet = sheet[:i] + generated + sheet[i:]
    open(args.sch, 'w').write(sheet)
    print(f"placed {args.pair}: {[c['ref'] for c in chips]}, passives {list(ref.values())}, "
          f"power {pwr_refs[0]}..{pwr_refs[-1]} into {args.sch}")


if __name__ == '__main__':
    main()
