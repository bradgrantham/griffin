#!/usr/bin/env python3
"""Place an IDT7200 FIFO pair, with bypass caps, /R series resistors and
global labels, on a KiCad 9 sheet.

Each pair is described by an entry in PAIRS below (chip references, sheet
positions, data-bus byte, read-strobe net, capacitor and resistor references).
Pin handling per lib pin name:
  D0..D7 -> bus label (input)       D8 -> GND
  Q0..Q7 -> Q bus label (tri_state) Q8 -> no-connect
  ~{W}   -> write strobe label      ~{RS} -> reset label
  ~{R}   -> series resistor, then the read strobe label on the driver side
  ~{XI}  -> GND (single device mode) ~{FL}/~{RT} -> +5V (retransmit held high)
  ~{EF}, ~{FF}, ~{HF}/~{XO} -> no-connect
  VCC -> +5V, GND -> GND power symbols on the pin ends

Uses the KiCad Memory_RAM:IDT7201 symbol (identical DIP-28 pinout to the
7200) and Device:R / Device:C / power:+5V / power:GND.  Lib symbols are copied
into the sheet's lib_symbols when absent.

Example:  place_fifo.py --sch pixel.kicad_sch --pair pixels
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

PAIRS = {
    'pixels': dict(
        w='~{PIXELS_FIFO_W}', rs='~{RS}', q='PIXELS_Q', series='47R', pwr_start=153,
        chips=[
            dict(ref='U22', at=(210.82, 55.88), d_lo=8, re='~{PIXELS_RE_EVEN}', res='R28', cap='C29', cap_at=(177.8, 78.74)),
            dict(ref='U23', at=(210.82, 129.54), d_lo=0, re='~{PIXELS_RE_ODD}', res='R29', cap='C30', cap_at=(177.8, 152.4)),
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


def lib_symbol_block(libname, name):
    """The `(symbol "NAME" ...)` block from KiCad's library, re-keyed and re-indented for a sheet."""
    path = os.path.join(KICAD_SYMS, libname + '.kicad_sym')
    text = open(path).read()
    span = block_span(text, r'^\t\(symbol "' + re.escape(name) + r'"\n', )
    if not span:
        sys.exit(f"{name} not found in {path}")
    blk = text[span[0]:span[1]]
    blk = blk.replace(f'(symbol "{name}"', f'(symbol "{libname}:{name}"', 1)
    return '\n'.join('\t' + l if l else l for l in blk.split('\n')) + '\n'


def ensure_lib_symbol(sheet, libname, name):
    lib_id = f'{libname}:{name}'
    if f'(symbol "{lib_id}"' in sheet:
        return sheet
    s, e = block_span(sheet, r'^\t\(lib_symbols')
    blk = lib_symbol_block(libname, name)
    # insert before the closing paren of lib_symbols (which sits on its own line "\t)")
    close = sheet.rfind('\n\t)', s, e)
    return sheet[:close + 1] + blk.rstrip('\n') + sheet[close:]


def lib_pins(sheet, lib_id):
    """[(number, name, lx, ly, rot)] of a lib symbol already present in the sheet."""
    root = parse_sexp(sheet)[0]
    for sym in find_all(find_one(root, 'lib_symbols'), 'symbol'):
        if atom(sym[1]) != lib_id:
            continue
        pins = []
        for unit in find_all(sym, 'symbol'):
            for pin in find_all(unit, 'pin'):
                at = find_one(pin, 'at')
                pins.append((atom(find_one(pin, 'number')[1]), atom(find_one(pin, 'name')[1]),
                             float(atom(at[1])), float(atom(at[2])), int(float(atom(at[3])))))
        return pins
    sys.exit(f"{lib_id} missing from lib_symbols")


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


def instance_block(lib_id, x, y, rot, ref, props, pin_numbers, path):
    out = f'\t(symbol\n\t\t(lib_id "{lib_id}")\n\t\t(at {fmt(x)} {fmt(y)} {rot})\n\t\t(unit 1)\n'
    out += '\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)\n\t\t(fields_autoplaced yes)\n'
    out += f'\t\t(uuid "{uuid.uuid4()}")\n'
    out += ''.join(props)
    for n in pin_numbers:
        out += f'\t\t(pin "{n}"\n\t\t\t(uuid "{uuid.uuid4()}")\n\t\t)\n'
    out += f'\t\t(instances\n\t\t\t(project "board"\n\t\t\t\t(path "{path}"\n\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n'
    return out


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


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sch', required=True)
    ap.add_argument('--pair', required=True, choices=sorted(PAIRS))
    args = ap.parse_args()
    cfg = PAIRS[args.pair]

    sheet = open(args.sch).read()
    root = parse_sexp(sheet)[0]

    # instance path of this sheet, from any existing symbol
    paths = set(re.findall(r'\(path "(/[0-9a-f-]+/[0-9a-f-]+)"', sheet))
    if len(paths) != 1:
        sys.exit(f"expected one instance path in {args.sch}, found {paths}")
    path = paths.pop()

    # reference clash check across the project
    used = set()
    for f in glob.glob(os.path.join(os.path.dirname(os.path.abspath(args.sch)), '*.kicad_sch')):
        used |= set(re.findall(r'\(reference "([^"]+)"', open(f).read()))
    # per chip: VCC, GND, /XI, D8, /FL ties, plus the bypass cluster's +5V and GND
    pwr_refs = [f"#PWR{cfg['pwr_start'] + i:04d}" for i in range(7 * len(cfg['chips']))]
    wanted = [c['ref'] for c in cfg['chips']] + [c['res'] for c in cfg['chips']] + [c['cap'] for c in cfg['chips']]
    clash = sorted(set(wanted + pwr_refs) & used)
    if clash:
        sys.exit(f"references already in use: {clash}")

    for part in (FIFO, RES, CAP):
        sheet = ensure_lib_symbol(sheet, part['lib'], part['name'])
    for kind in ('+5V', 'GND'):
        if f'(symbol "power:{kind}"' not in sheet:
            sheet = ensure_lib_symbol(sheet, 'power', kind)

    fifo_id = f"{FIFO['lib']}:{FIFO['name']}"
    pins = lib_pins(sheet, fifo_id)
    pin_numbers = [p[0] for p in pins]

    out = []
    pwr = iter(pwr_refs)
    for chip in cfg['chips']:
        sx, sy = chip['at']
        props = [
            prop('Reference', chip['ref'], sx - 2.54, sy - 23.495, justify='right'),
            prop('Value', FIFO['value'], sx - 2.54, sy - 20.955, justify='right'),
            prop('Footprint', FIFO['footprint'], sx, sy, hide=True),
            prop('Datasheet', FIFO['datasheet'], sx, sy, hide=True),
            prop('Description', FIFO['description'], sx, sy, hide=True),
        ]
        out.append(instance_block(fifo_id, sx, sy, 0, chip['ref'], props, pin_numbers, path))

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
                out += [wire_block(px, py, ex, py), label_block(f"D{chip['d_lo'] + int(m.group(1))}", 'input', ex, py, angle)]
                continue
            m = re.fullmatch(r'Q(\d)', name)
            if m and int(m.group(1)) < 8:
                out += [wire_block(px, py, ex, py), label_block(f"{cfg['q']}{m.group(1)}", 'tri_state', ex, py, angle)]
                continue
            if name == '~{W}':
                out += [wire_block(px, py, ex, py), label_block(cfg['w'], 'input', ex, py, angle)]
            elif name == '~{RS}':
                out += [wire_block(px, py, ex, py), label_block(cfg['rs'], 'input', ex, py, angle)]
            elif name == '~{R}':
                # pin -> 2.54 wire -> resistor (7.62 long, horizontal) -> 2.54 wire -> label
                x1 = px + away * 2.54
                rc = x1 + away * 3.81
                x2 = x1 + away * 7.62
                x3 = x2 + away * 2.54
                out += [wire_block(px, py, x1, py),
                        two_pin_block(RES, rc, py, 90, chip['res'], cfg['series'], path),
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
        out += [two_pin_block(CAP, cx, cy, 0, chip['cap'], CAP['value'], path),
                power_block('+5V', cx, cy - 3.81, 0, next(pwr), path),
                power_block('GND', cx, cy + 3.81, 0, next(pwr), path)]

    if next(pwr, None) is not None:
        sys.exit("power reference count mismatch")
    used_pwr = pwr_refs
    generated = ''.join(out)
    i = sheet.rstrip().rfind(')')
    sheet = sheet[:i] + generated + sheet[i:]
    open(args.sch, 'w').write(sheet)
    print(f"placed {args.pair}: {[c['ref'] for c in cfg['chips']]}, caps {[c['cap'] for c in cfg['chips']]}, "
          f"resistors {[c['res'] for c in cfg['chips']]}, power {used_pwr[0]}..{used_pwr[-1]} into {args.sch}")


if __name__ == '__main__':
    main()
