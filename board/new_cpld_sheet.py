#!/usr/bin/env python3
"""Clone a KiCad 9 sheet file into a new sheet and register it on the root sheet.

Meant for the CPLD sheets: the template is an existing sheet holding a CPLD
symbol, its bypass capacitor and power symbols (e.g. the ENGINE sheet before it
was labeled).  Every uuid is regenerated, symbol instance paths are re-pointed
at the new sheet, references are renamed per --refs, the CPLD's Value is set,
a (sheet ...) block is appended to the root, and the project file's "sheets"
list gets an entry.  Then run label_cpld.py on the new file.

Example:
  new_cpld_sheet.py --template engine_template.kicad_sch --out pixel.kicad_sch \
      --root board.kicad_sch --sheetname PIXEL --value PIXEL \
      --refs U13=U20,C18=C27,#PWR0116=#PWR0145,#PWR0115=#PWR0146,#PWR097=#PWR0147,#PWR0106=#PWR0148 \
      --at 215.9 101.6 --size 40 20 --page 13
"""

import argparse
import glob
import json
import os
import re
import sys
import uuid


def fmt(v):
    s = f"{v:.4f}".rstrip('0').rstrip('.')
    return s if s not in ('-0', '') else '0'


def sheet_block(sheet_uuid, root_uuid, name, fname, x, y, w, h, page):
    return f'''	(sheet
		(at {fmt(x)} {fmt(y)})
		(size {fmt(w)} {fmt(h)})
		(exclude_from_sim no)
		(in_bom yes)
		(on_board yes)
		(dnp no)
		(fields_autoplaced yes)
		(stroke
			(width 0.1524)
			(type solid)
		)
		(fill
			(color 0 0 0 0.0000)
		)
		(uuid "{sheet_uuid}")
		(property "Sheetname" "{name}"
			(at {fmt(x)} {fmt(y - 0.7116)} 0)
			(effects
				(font
					(size 1.27 1.27)
				)
				(justify left bottom)
			)
		)
		(property "Sheetfile" "{fname}"
			(at {fmt(x)} {fmt(y + h + 3.4446)} 0)
			(effects
				(font
					(size 1.27 1.27)
				)
				(justify left top)
			)
		)
		(instances
			(project "board"
				(path "/{root_uuid}"
					(page "{page}")
				)
			)
		)
	)
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--template', required=True)
    ap.add_argument('--out', required=True, help='new sheet file, e.g. pixel.kicad_sch (must not exist)')
    ap.add_argument('--root', required=True, help='root schematic, e.g. board.kicad_sch')
    ap.add_argument('--sheetname', required=True)
    ap.add_argument('--value', required=True, help='Value for the template symbol whose Value is --old-value')
    ap.add_argument('--old-value', default=None, help='template Value to replace (default: the first U reference symbol)')
    ap.add_argument('--refs', required=True, help='comma list OLD=NEW for every reference in the template')
    ap.add_argument('--at', nargs=2, type=float, required=True, metavar=('X', 'Y'))
    ap.add_argument('--size', nargs=2, type=float, default=(40, 20), metavar=('W', 'H'))
    ap.add_argument('--page', required=True)
    args = ap.parse_args()

    if os.path.exists(args.out):
        sys.exit(f"{args.out} already exists")
    text = open(args.template).read()
    root = open(args.root).read()
    root_uuid = re.search(r'^\t\(uuid "([0-9a-f-]+)"\)', root, re.M).group(1)

    refmap = dict(kv.split('=', 1) for kv in args.refs.split(','))
    used = set()
    for f in glob.glob(os.path.join(os.path.dirname(args.root) or '.', '*.kicad_sch')):
        used |= set(re.findall(r'\(reference "([^"]+)"', open(f).read()))
    clash = sorted(set(refmap.values()) & used)
    if clash:
        sys.exit(f"target references already in use: {clash}")
    have = set(re.findall(r'\(property "Reference" "([^"]+)"', text)) - {'U', 'C', '#PWR', 'R'}
    missing = sorted(have - set(refmap))
    if missing:
        sys.exit(f"template references without a mapping: {missing}")

    # Sheet path: template symbols carry (path "/<root>/<template sheet uuid>")
    paths = set(re.findall(r'\(path "(/[0-9a-f-]+/[0-9a-f-]+)"', text))
    if len(paths) != 1:
        sys.exit(f"expected one instance path in the template, found {paths}")
    new_sheet = str(uuid.uuid4())
    text = text.replace(paths.pop(), f'/{root_uuid}/{new_sheet}')

    # Fresh uuid for every element (file uuid included)
    text = re.sub(r'\(uuid "[0-9a-f-]+"\)', lambda m: f'(uuid "{uuid.uuid4()}")', text)

    # References
    for old, new in refmap.items():
        text, n1 = re.subn(r'\(property "Reference" "' + re.escape(old) + '"', f'(property "Reference" "{new}"', text)
        text, n2 = re.subn(r'\(reference "' + re.escape(old) + '"\)', f'(reference "{new}")', text)
        if n1 != 1 or n2 < 1:
            sys.exit(f"reference {old}: {n1} property / {n2} instance matches")

    # Value of the CPLD symbol
    if args.old_value is None:
        u = [n for o, n in refmap.items() if o.startswith('U')]
        if len(u) != 1:
            sys.exit("give --old-value; template has %d U references" % len(u))
        m = re.search(r'\(property "Reference" "' + re.escape(u[0]) + r'"(?:[^\n]*\n)*?\t\t\(property "Value" "([^"]*)"', text)
        args.old_value = m.group(1)
    text, n = re.subn(r'\(property "Value" "' + re.escape(args.old_value) + '"', f'(property "Value" "{args.value}"', text)
    if n != 1:
        sys.exit(f"Value {args.old_value!r}: {n} matches")

    open(args.out, 'w').write(text)

    # Root sheet block
    fname = os.path.basename(args.out)
    if f'"{fname}"' in root:
        sys.exit(f"{args.root} already references {fname}")
    block = sheet_block(new_sheet, root_uuid, args.sheetname, fname, *args.at, *args.size, args.page)
    i = root.rstrip().rfind(')')
    root = root[:i] + block + root[i:]
    open(args.root, 'w').write(root)

    # Project file "sheets" list (KiCad rewrites it on save; keep it consistent)
    pro = os.path.splitext(args.root)[0] + '.kicad_pro'
    if os.path.exists(pro):
        ptext = open(pro).read()
        m = re.search(r'("sheets": \[(?:.|\n)*?\n    \])(\n  \],)', ptext)
        if m:
            entry = f',\n    [\n      "{new_sheet}",\n      "{args.sheetname}"\n    ]'
            ptext = ptext[:m.end(1)] + entry + ptext[m.end(1):]
            open(pro, 'w').write(ptext)
        else:
            j = json.load(open(pro))
            j.setdefault('sheets', []).append([new_sheet, args.sheetname])
            json.dump(j, open(pro, 'w'), indent=2)
            open(pro, 'a').write('\n')

    print(f"wrote {args.out} (sheet uuid {new_sheet}), added {args.sheetname} page {args.page} to {args.root}")


if __name__ == '__main__':
    main()
