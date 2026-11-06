#!/usr/bin/env python3
"""Check every CPLD in cpld_map.py against the KiCad netlist.

Exports the netlist of board.kicad_sch with kicad-cli, then for each chip
builds the expected pin -> net map exactly as label_cpld.py --chip would
(label_cpld.chip_assignment) and compares it with the netlist:
  global labels   the net name must match exactly
  local labels    KiCad names the net /SHEET/NAME; the leaf name must match
  no-connect      the pin must be on an unconnected-(...) net or absent
and checks the power pins (VCC on +5V, GND on GND).  Every difference prints
as `CHIP pin N: expected X, found Y`.

Then, for information, lists every net with exactly one pin (other than
KiCad's unconnected-(...) nets) and every global label that appears on only
one sheet file of the hierarchy.

Exit status 1 on any CPLD mismatch.  A chip whose sheet is missing is reported
and fails only with --strict; with --strict the dangling lists fail too.

Example:
  check_cpld_pins.py
  check_cpld_pins.py --board-dir /tmp/scratch/board --chip PORTS
"""

import argparse
import os
import subprocess
import sys
import tempfile
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cpld_map  # noqa: E402
from label_cpld import parse_sexp, find_all, find_one, atom, chip_assignment, chip_sheet  # noqa: E402
from kicad_netlist_summary_2 import parse_netlist, parse_sexpr_all, find_children, find_child  # noqa: E402

KICAD_CLI = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'


def export_netlist(board_dir, out):
    cmd = [KICAD_CLI, 'sch', 'export', 'netlist', '--format', 'kicadsexpr', '-o', out,
           os.path.join(board_dir, 'board.kicad_sch')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        sys.exit(f"kicad-cli failed ({r.returncode}):\n{r.stdout}{r.stderr}")


def component_sheetfiles(path):
    """{ref: Sheetfile} from the netlist's components section."""
    tree = parse_sexpr_all(open(path).read())
    result = {}
    for comp in find_children(find_child(tree, 'components') or [], 'comp'):
        ref = find_child(comp, 'ref')
        for prop in find_children(comp, 'property'):
            name, value = find_child(prop, 'name'), find_child(prop, 'value')
            if name and value and name[1] == 'Sheetfile' and ref:
                result[ref[1]] = value[1]
    return result


def describe(net):
    return '(unconnected)' if net is None else net


def check_chip(name, board_dir, pin_nets, sheetfiles):
    """Return (list of mismatch strings, sheet used or None)."""
    chip = cpld_map.CHIPS[name]
    sch = chip_sheet(chip, board_dir)
    if sch is None:
        return None, None
    ref = chip['ref']
    problems = []
    found_sheet = sheetfiles.get(ref)
    if found_sheet is None:
        return [f"{name}: {ref} not in the netlist"], sch
    if found_sheet != os.path.basename(sch):
        problems.append(f"{name}: {ref} is on {found_sheet}, map says {os.path.basename(sch)}")

    for num, (kind, net, _shape) in sorted(chip_assignment(name, board_dir).items()):
        got = pin_nets.get((ref, str(num)))
        if got is not None and got.startswith('unconnected-('):
            got = None
        if kind == 'nc':
            ok = got is None
            want = '(unconnected)'
        elif kind == 'local':
            ok = got is not None and got.startswith('/') and got.rsplit('/', 1)[1] == net
            want = f"local {net}"
        else:
            ok = got == net
            want = net
        if not ok:
            problems.append(f"{name} pin {num}: expected {want}, found {describe(got)}")
    for num, net in sorted(cpld_map.POWER_PINS.items()):
        got = pin_nets.get((ref, str(num)))
        if got != net:
            problems.append(f"{name} pin {num}: expected {net}, found {describe(got)}")
    return problems, sch


def sheet_files(board_dir):
    """Sheet files of the hierarchy under board.kicad_sch: (present, missing)."""
    seen, missing, todo = [], [], ['board.kicad_sch']
    while todo:
        f = todo.pop(0)
        if f in seen or f in missing:
            continue
        path = os.path.join(board_dir, f)
        if not os.path.exists(path):
            missing.append(f)
            continue
        seen.append(f)
        root = parse_sexp(open(path).read())[0]
        for sheet in find_all(root, 'sheet'):
            for prop in find_all(sheet, 'property'):
                if atom(prop[1]) == 'Sheetfile':
                    todo.append(atom(prop[2]))
    return seen, missing


def lonely_global_labels(board_dir):
    files, missing = sheet_files(board_dir)
    where = defaultdict(set)
    for f in files:
        root = parse_sexp(open(os.path.join(board_dir, f)).read())[0]
        for lb in find_all(root, 'global_label'):
            where[atom(lb[1])].add(f)
    return sorted((n, next(iter(s))) for n, s in where.items() if len(s) == 1), missing


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--board-dir', default=HERE, help='directory holding board.kicad_sch (default: this script\'s)')
    ap.add_argument('--chip', action='append', help='check only this chip (repeatable)')
    ap.add_argument('--netlist', help='use this netlist instead of exporting one')
    ap.add_argument('--strict', action='store_true',
                    help='missing sheets and dangling nets / lonely global labels also fail')
    args = ap.parse_args()
    board_dir = os.path.abspath(args.board_dir)

    if args.netlist:
        net_path = args.netlist
    else:
        net_path = os.path.join(tempfile.mkdtemp(prefix='check_cpld_pins.'), 'board.net')
        export_netlist(board_dir, net_path)
    _, nets = parse_netlist(net_path)
    pin_nets = {}
    for net, nodes in nets.items():
        for n in nodes:
            pin_nets[(n['ref'], n['pin'])] = net
    sheetfiles = component_sheetfiles(net_path)

    failed = False
    chips = args.chip or list(cpld_map.CHIPS)
    for name in chips:
        if name not in cpld_map.CHIPS:
            sys.exit(f"unknown chip {name}; known: {sorted(cpld_map.CHIPS)}")
        problems, sch = check_chip(name, board_dir, pin_nets, sheetfiles)
        if problems is None:
            print(f"{name}: sheet missing ({cpld_map.CHIPS[name]['sheet']})")
            failed |= args.strict
            continue
        print(f"{name}: {cpld_map.CHIPS[name]['ref']} on {os.path.basename(sch)}: {len(problems)} mismatches")
        for p in problems:
            print(f"  {p}")
        failed |= bool(problems)

    single = sorted((net, nodes[0]) for net, nodes in nets.items()
                    if len(nodes) == 1 and not net.startswith('unconnected-('))
    print(f"\nnets with exactly one pin: {len(single)}")
    for net, n in single:
        print(f"  {net:<40} {n['ref']} pin {n['pin']}")
    lonely, missing = lonely_global_labels(board_dir)
    if missing:
        print(f"\nsheet files referenced but missing: {', '.join(missing)}")
    print(f"\nglobal labels on only one sheet: {len(lonely)}")
    for name, f in lonely:
        print(f"  {name:<40} {f}")
    if args.strict and (single or lonely):
        failed = True

    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
