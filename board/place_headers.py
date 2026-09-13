#!/usr/bin/env python3
"""Place the two logic-analyzer debug headers on a sheet.

Both headers mirror the input connector (J2) of the gusmanb LogicAnalyzer V2
board (Electronics/LogicAnalyzer/LogicAnalyzerV2, JITX design): a shrouded
2x16 box header wired straight through by a 32-way IDC ribbon, so Griffin's
pin n is the analyzer's pin n:

   1,2 GND      3 EXT_VREF (Griffin +5V: the analyzer's "ext" reference)
   4 EXT_TRIG (no-connect)   5,6 analyzer 5 V, 7,8 analyzer 3V3 (no-connect)
   9..32 channels: odd pins carry DIN12 down to DIN1 (pin 31), even pins
   DIN13 up to DIN24 (pin 32).  DINk is analyzer channel k.

Header 1 "what happened": channels 1-16 = D0..D15, 17-23 = ~AS ~UDS ~LDS
R/~W ~DTACK ~RESET ~HALT, 24 spare.  Header 2 "where": channels 1-23 =
A1..A23, 24 = SYSCLK through a series resistor (never a bare stub on the
clock net).

Example:  place_headers.py --sch debug-header.kicad_sch
"""

import argparse
import glob
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from label_cpld import fmt, wire_block, label_block, nc_block  # noqa: E402
from place_fifo import (ensure_lib_symbol, lib_pins, instance_block, prop, power_block,  # noqa: E402
                        two_pin_block, local_label_block, RES)

STUB = 5.08
CONN = dict(lib='Connector_Generic', name='Conn_02x16_Odd_Even', value='DEBUG_HEADER',
            footprint='board:IDC-Header_2x16_P2.54mm_Vertical',
            description='Logic analyzer debug header, gusmanb LogicAnalyzer V2 J2 pinout, 2x16 IDC box header')

# analyzer pin -> DIN channel (1-based), from the V2 design's main.stanza
DIN_PIN = {}
for i in range(24):
    DIN_PIN[31 - 2 * i if i < 12 else 32 - 2 * (i - 12)] = (i + 1) if i < 12 else (24 - (i - 12))

HEADERS = [
    dict(ref='J8', at=(63.5, 88.9), channels=[f'D{i}' for i in range(16)] +
         ['~{AS}', '~{UDS}', '~{LDS}', 'R{slash}~{W}', '~{DTACK}', '~{RESET}', '~{HALT}', None]),
    dict(ref='J9', at=(177.8, 88.9), channels=[f'A{i}' for i in range(1, 24)] + ['SYSCLK'],
         series=dict(channel=24, ref='R32', value='100R', local='SYSCLK_TAP')),
]
PWR_START = 195

NOTE = ('Debug headers mirror the gusmanb LogicAnalyzer V2 input connector (2x16 shrouded, 32-way IDC ribbon straight through).\\n'
        'DINk = analyzer channel k.  Pins 5-8 are the analyzer\'s own 5 V / 3V3 and stay open; pin 3 feeds its EXT_VREF from +5V.\\n'
        'J8: ch 1-16 D0-D15, 17-23 ~AS ~UDS ~LDS R/~W ~DTACK ~RESET ~HALT, 24 spare.  J9: ch 1-23 A1-A23, 24 SYSCLK via R32.')


def text_block(text, x, y):
    return f'''	(text "{text}"
		(exclude_from_sim no)
		(at {fmt(x)} {fmt(y)} 0)
		(effects
			(font
				(size 1.27 1.27)
			)
			(justify left bottom)
		)
		(uuid "{uuid.uuid4()}")
	)
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sch', required=True)
    args = ap.parse_args()
    sheet = open(args.sch).read()

    # sheet path from the root (the sheet starts empty)
    root_file = os.path.join(os.path.dirname(os.path.abspath(args.sch)), 'board.kicad_sch')
    rtext = open(root_file).read()
    root_uuid = re.search(r'^\t\(uuid "([0-9a-f-]+)"\)', rtext, re.M).group(1)
    m = re.search(r'\(uuid "([0-9a-f-]+)"\)\n\t\t\(property "Sheetname" "[^"]*"\n(?:[^\n]*\n)*?\t\t\(property "Sheetfile" "'
                  + re.escape(os.path.basename(args.sch)) + '"', rtext)
    if not m:
        sys.exit(f"{root_file} has no sheet block for {os.path.basename(args.sch)}")
    path = f'/{root_uuid}/{m.group(1)}'

    used = set()
    for f in glob.glob(os.path.join(os.path.dirname(os.path.abspath(args.sch)), '*.kicad_sch')):
        used |= set(re.findall(r'\(reference "([^"]+)"', open(f).read()))
    n_pwr = sum(3 for h in HEADERS)          # pins 1, 2 -> GND, pin 3 -> +5V
    pwr_refs = [f"#PWR{PWR_START + i:04d}" for i in range(n_pwr)]
    wanted = [h['ref'] for h in HEADERS] + [h['series']['ref'] for h in HEADERS if 'series' in h] + pwr_refs
    clash = sorted(set(wanted) & used)
    if clash:
        sys.exit(f"references already in use: {clash}")

    sheet = ensure_lib_symbol(sheet, CONN['lib'], CONN['name'])
    for lib, name in ((RES['lib'], RES['name']), ('power', '+5V'), ('power', 'GND')):
        sheet = ensure_lib_symbol(sheet, lib, name)
    conn_id = f"{CONN['lib']}:{CONN['name']}"
    pins = lib_pins(sheet, conn_id)

    out = []
    pwr = iter(pwr_refs)
    for h in HEADERS:
        sx, sy = h['at']
        props = [
            prop('Reference', h['ref'], sx + 1.27, sy - 22.86),
            prop('Value', CONN['value'], sx + 1.27, sy + 22.86),
            prop('Footprint', CONN['footprint'], sx, sy, hide=True),
            prop('Datasheet', 'https://github.com/gusmanb/logicanalyzer/wiki/02---LogicAnalyzer-Hardware', sx, sy, hide=True),
            prop('Description', CONN['description'], sx, sy, hide=True),
        ]
        out.append(instance_block(conn_id, sx, sy, 0, h['ref'], props, [p[0] for p in pins], path))
        for number, name, lx, ly, rot in pins:
            n = int(number)
            px, py = sx + lx, sy - ly
            if rot == 0:
                ex, angle, away = px - STUB, 180, -1
            else:
                ex, angle, away = px + STUB, 0, +1
            if n in (1, 2):
                out += [wire_block(px, py, ex, py), power_block('GND', ex, py, 270 if away < 0 else 90, next(pwr), path)]
            elif n == 3:
                out += [wire_block(px, py, ex, py), power_block('+5V', ex, py, 90 if away < 0 else 270, next(pwr), path)]
            elif n in (4, 5, 6, 7, 8):
                out.append(nc_block(px, py))
            else:
                ch = DIN_PIN[n]
                net = h['channels'][ch - 1]
                if net is None:
                    out.append(nc_block(px, py))
                elif 'series' in h and h['series']['channel'] == ch:
                    # long stub past the neighbouring labels; the pin-side net gets a local
                    # label because KiCad's netlist export drops an unnamed net between two
                    # passive pins (verified 2026-09-12)
                    x1 = px + away * 12.7
                    rc = x1 + away * 3.81
                    x2 = x1 + away * 7.62
                    x3 = x2 + away * 2.54
                    out += [wire_block(px, py, x1, py),
                            local_label_block(h['series']['local'], px + away * 1.27, py, 0 if away > 0 else 180),
                            two_pin_block(RES, rc, py, 90, h['series']['ref'], h['series']['value'], path),
                            wire_block(x2, py, x3, py),
                            label_block(net, 'passive', x3, py, angle)]
                else:
                    out += [wire_block(px, py, ex, py), label_block(net, 'passive', ex, py, angle)]
    if next(pwr, None) is not None:
        sys.exit("power reference count mismatch")
    out.append(text_block(NOTE, 25.4, 160.02))

    i = sheet.rstrip().rfind(')')
    sheet = sheet[:i] + ''.join(out) + sheet[i:]
    open(args.sch, 'w').write(sheet)
    print(f"placed {[h['ref'] for h in HEADERS]}, power {pwr_refs[0]}..{pwr_refs[-1]} into {args.sch}")


if __name__ == '__main__':
    main()
