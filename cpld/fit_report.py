#!/usr/bin/env python3
"""
fit_report.py — Parse ATF1508 fitter output (.fit) and yosys EDIF (.edif)
to produce a readable placement, foldback, and fan-in report with Verilog
signal names instead of opaque fitter IDs.

Usage:
    python3 fit_report.py <design>.fit <design>.edif

The .fit file is produced by fit1508.exe (via run_fitter.sh).
The .edif file is produced by yosys (via run_yosys.sh).
"""

import sys
import re
import argparse
from collections import defaultdict


# ---------------------------------------------------------------------------
# EDIF parsing — extract idXXXXX → Verilog signal name mapping
# ---------------------------------------------------------------------------

def parse_edif_names(edif_path):
    """Parse EDIF to build a mapping from fitter instance/net IDs to
    human-readable Verilog signal names.

    Returns dict: bare ID (e.g. 'id00340') → signal name (e.g. 'rom_overlay_disable')

    For nets with ABC-generated names ($abc$...$new_nXXX), we keep a short
    form like 'abc:nXXX' rather than the full mangled path.
    """
    id_to_name = {}

    with open(edif_path) as f:
        edif_text = f.read()

    lines = edif_text.split('\n')
    current_net_name = None
    in_net = False

    for line in lines:
        stripped = line.strip()

        # Detect net start with rename
        m = re.match(r'\(net\s+\(rename\s+\S+\s+"([^"]+)"\)\s+\(joined', stripped)
        if m:
            current_net_name = m.group(1)
            # Shorten ABC-internal names to something readable
            abc = re.match(r'\$abc\$\d+\$new_n(\d+)', current_net_name)
            if abc:
                current_net_name = f"abc:n{abc.group(1)}"
            in_net = True
            continue

        # Detect net start without rename
        m = re.match(r'\(net\s+(\S+)\s+\(joined', stripped)
        if m:
            current_net_name = m.group(1)
            in_net = True
            continue

        if in_net:
            # Look for (portRef Q (instanceRef idXXXXX))
            m = re.search(r'\(portRef\s+Q\s+\(instanceRef\s+(id\d+)\)', stripped)
            if m and current_net_name:
                fitter_id = m.group(1)
                id_to_name[fitter_id] = current_net_name

            # Also capture (portRef QN ...) for inverted outputs
            m = re.search(r'\(portRef\s+QN\s+\(instanceRef\s+(id\d+)\)', stripped)
            if m and current_net_name:
                fitter_id = m.group(1)
                if fitter_id not in id_to_name:
                    id_to_name[fitter_id] = current_net_name + " (QN)"

            # Detect net end
            if ')' in stripped and stripped.count(')') > stripped.count('('):
                in_net = False
                current_net_name = None

    return id_to_name


# ---------------------------------------------------------------------------
# .fit parsing
# ---------------------------------------------------------------------------

def parse_fit_file(fit_path):
    """Parse the fitter .fit file and extract structured information.

    Returns a dict with keys:
        'placements': list of (signal, placement_type, mc_num, detail)
        'foldbacks': list of (signal, node_id, mc_num)
        'fan_in': dict of block_letter → {'count': N, 'signals': [list]}
        'equations': list of (output_name, equation_text, product_term_count)
        'summary': dict of stat_name → value string
    """
    with open(fit_path) as f:
        lines = f.readlines()

    placements = []
    foldbacks = []
    fan_in = {}
    equations = []
    summary = {}

    i = 0
    while i < len(lines):
        line = lines[i].rstrip()

        # --- Placement: "XXX is placed at pin/feedback/foldback ..." ---
        m = re.match(
            r'(\S+)\s+is placed at (pin|feedback node|foldback expander node)\s+(\d+)\s+\(MC\s+(\d+)\)',
            line)
        if m:
            signal = m.group(1)
            ptype = m.group(2)
            node = int(m.group(3))
            mc = int(m.group(4))
            placements.append((signal, ptype, mc, node))
            if ptype == 'foldback expander node':
                foldbacks.append((signal, node, mc))
            i += 1
            continue

        # --- Fan-in blocks: "FanIn assignment for block X [N]" ---
        m = re.match(r'FanIn assignment for block (\w)\s+\[(\d+)\]', line)
        if m:
            block = m.group(1)
            count = int(m.group(2))
            # Collect signals until we hit '}'
            signals = []
            i += 1
            while i < len(lines):
                fline = lines[i].rstrip()
                if fline.strip() == '{':
                    i += 1
                    continue
                if fline.strip() == '}':
                    i += 1
                    break
                # Parse comma-separated signal names
                for sig in fline.strip().rstrip(',').split(','):
                    sig = sig.strip()
                    if sig:
                        signals.append(sig)
                i += 1
            fan_in[block] = {'count': count, 'signals': signals}
            continue

        # --- Equations: "signal = (...)" or "signal.D = (...)" ---
        m = re.match(r'([\w.]+)\s*=\s*(.*)', line)
        if m and i > 600:  # equations are in the latter part of the file
            eq_name = m.group(1)
            eq_body = m.group(2)
            # Multi-line equations: continue while next line starts with tab/whitespace and '#'
            while i + 1 < len(lines) and re.match(r'\s+[#\(]', lines[i + 1]):
                i += 1
                eq_body += ' ' + lines[i].strip()
            # Count product terms (# separates OR'd terms in ABEL)
            pt_count = eq_body.count('#') + 1 if eq_body.strip() else 0
            # Simple constant assignments
            if re.match(r'^[01];?\s*$', eq_body.strip()):
                pt_count = 0
            equations.append((eq_name, eq_body.rstrip(';').strip(), pt_count))
            i += 1
            continue

        # --- Summary stats ---
        m = re.match(r'Total (Logic cells|Flip-Flop|Foldback logic|Nodes\+FB/MCells|Pts|'
                     r'dedicated input|I/O pins|cascade|input pins|output pins)\s+.*?'
                     r'(\d+(?:/\d+)?(?:\s+\(\d+%\))?)', line)
        if m:
            summary[m.group(1)] = m.group(2).strip()
            i += 1
            continue

        i += 1

    return {
        'placements': placements,
        'foldbacks': foldbacks,
        'fan_in': fan_in,
        'equations': equations,
        'summary': summary,
    }


# ---------------------------------------------------------------------------
# Signal name resolution
# ---------------------------------------------------------------------------

def resolve_name(signal, id_map):
    """Resolve a fitter signal name to a Verilog name using the EDIF map."""
    # Strip suffixes like Q, QN for lookup
    bare = re.sub(r'(Q|QN)$', '', signal)
    if bare in id_map:
        suffix = signal[len(bare):]
        name = id_map[bare]
        if suffix == 'QN' and '(QN)' not in name:
            return name + " (inv)"
        return name
    # XXL_ and Com_Ctrl_ are yosys-internal; just return as-is
    return signal


def resolve_signals(signals, id_map):
    """Resolve a list of signals, returning list of (original, resolved) tuples."""
    return [(s, resolve_name(s, id_map)) for s in signals]


# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------

LAB_LETTERS = 'ABCDEFGH'
MC_RANGES = {
    'A': (1, 16), 'B': (17, 32), 'C': (33, 48), 'D': (49, 64),
    'E': (65, 80), 'F': (81, 96), 'G': (97, 112), 'H': (113, 128),
}


def mc_to_lab(mc):
    for lab, (lo, hi) in MC_RANGES.items():
        if lo <= mc <= hi:
            return lab
    return '?'


def generate_report(fit_data, id_map):
    """Generate the human-readable report."""
    out = []
    w = out.append

    placements = fit_data['placements']
    foldbacks = fit_data['foldbacks']
    fan_in = fit_data['fan_in']
    equations = fit_data['equations']
    summary = fit_data['summary']

    # --- Header ---
    w("=" * 72)
    w("ATF1508 Fitter Report")
    w("=" * 72)
    w("")

    # --- Summary ---
    w("UTILIZATION SUMMARY")
    w("-" * 40)
    for k, v in summary.items():
        w(f"  {k:30s}  {v}")
    w("")

    # --- Per-LAB summary ---
    w("PER-LAB PLACEMENT")
    w("-" * 72)

    lab_placements = defaultdict(lambda: {'pins': [], 'feedback': [], 'foldback': []})
    for signal, ptype, mc, node in placements:
        lab = mc_to_lab(mc)
        name = resolve_name(signal, id_map)
        entry = f"MC{mc:3d}  {name}"
        if ptype == 'pin':
            lab_placements[lab]['pins'].append((mc, name, node))
        elif ptype == 'feedback node':
            lab_placements[lab]['feedback'].append((mc, name))
        else:
            lab_placements[lab]['foldback'].append((mc, name, signal))

    for lab in LAB_LETTERS:
        lo, hi = MC_RANGES[lab]
        lp = lab_placements[lab]
        pin_count = len(lp['pins'])
        fb_count = len(lp['feedback'])
        fold_count = len(lp['foldback'])
        total = pin_count + fb_count + fold_count
        fi = fan_in.get(lab, {})
        fi_count = fi.get('count', 0)

        w(f"  LAB {lab} (MC {lo}-{hi}):  {total}/16 used  "
          f"({pin_count} pin, {fb_count} feedback, {fold_count} foldback)  "
          f"fan-in: {fi_count}/40")

        for mc, name, pin in sorted(lp['pins']):
            w(f"    MC{mc:3d}  pin {pin:2d}  {name}")
        for mc, name in sorted(lp['feedback']):
            w(f"    MC{mc:3d}  feedback  {name}")
        for mc, name, orig in sorted(lp['foldback']):
            w(f"    MC{mc:3d}  FOLDBACK  {name}  [from {orig}]")
        w("")

    # --- Foldback detail ---
    w("FOLDBACK ANALYSIS")
    w("-" * 72)
    w(f"  Total foldbacks: {len(foldbacks)}")
    w("")

    # Build equation lookup and consumer index
    eq_by_name = {}
    for name, eq, pt in equations:
        eq_by_name[name] = (eq, pt)

    def resolve_eq(eq_text):
        """Resolve fitter IDs in an equation to Verilog names."""
        def replace_id(m):
            bare = re.sub(r'\.(Q|QN)$', '', m.group(0))
            bare_id = re.sub(r'(Q|QN)$', '', bare)
            r = id_map.get(bare_id, bare_id)
            if m.group(0).endswith('.QN'):
                return '!' + r
            return r
        result = re.sub(r'id\d+(?:\.(?:Q|QN))?', replace_id, eq_text)
        return re.sub(r'(XXL_\d+|Com_Ctrl_\d+)',
                       lambda m: resolve_name(m.group(0), id_map), result)

    if foldbacks:
        # Group foldbacks by source signal
        by_source = defaultdict(list)
        for signal, node, mc in foldbacks:
            by_source[signal].append((mc, mc_to_lab(mc)))

        w("  Signals replicated as foldbacks (most replicas first):")
        w("")
        for signal, locations in sorted(by_source.items(), key=lambda x: -len(x[1])):
            name = resolve_name(signal, id_map)
            labs = [lab for _, lab in locations]
            w(f"    {name:40s}  {len(locations)} copies  "
              f"in LABs: {', '.join(labs)}")

            # Show the equation for this foldback signal
            eq_key = '!' + signal if signal.endswith('QN') else signal
            # Try various forms
            for try_key in [signal, '!' + signal]:
                if try_key in eq_by_name:
                    eq_text, pt = eq_by_name[try_key]
                    w(f"      equation: {resolve_eq(eq_text)}")
                    break

            # Find which equations consume this signal
            consumers = []
            for eq_name, eq_text, pt in equations:
                if signal in eq_text:
                    eq_resolved = resolve_name(eq_name.split('.')[0], id_map)
                    suffix = eq_name[eq_name.index('.'):] if '.' in eq_name else ''
                    consumers.append(eq_resolved + suffix)
            if consumers:
                w(f"      consumed by: {', '.join(consumers[:8])}"
                  + (f" ... +{len(consumers)-8} more" if len(consumers) > 8 else ""))
            w("")

        # Which LABs are importing foldbacks?
        w("  Foldback imports per LAB:")
        w("")
        lab_imports = defaultdict(list)
        for signal, node, mc in foldbacks:
            lab = mc_to_lab(mc)
            name = resolve_name(signal, id_map)
            lab_imports[lab].append(name)

        for lab in LAB_LETTERS:
            imports = lab_imports.get(lab, [])
            if imports:
                w(f"    LAB {lab}: {len(imports)} foldback(s)")
                for name in imports:
                    w(f"      - {name}")
        w("")

    # --- Fan-in detail ---
    w("FAN-IN DETAIL")
    w("-" * 72)
    for lab in LAB_LETTERS:
        fi = fan_in.get(lab, {})
        count = fi.get('count', 0)
        signals = fi.get('signals', [])
        w(f"  LAB {lab}: {count}/40 inputs")
        if signals:
            resolved = resolve_signals(signals, id_map)
            # Categorize: pins vs internal
            pins = [(o, r) for o, r in resolved if not o.startswith('id') and not o.startswith('XXL') and not o.startswith('Com_')]
            internal = [(o, r) for o, r in resolved if o.startswith('id') or o.startswith('XXL') or o.startswith('Com_')]
            if pins:
                w(f"    I/O pins ({len(pins)}): {', '.join(r for _, r in pins)}")
            if internal:
                w(f"    Internal ({len(internal)}):")
                for orig, resolved_name in internal:
                    if resolved_name != orig:
                        w(f"      {resolved_name}")
                    else:
                        w(f"      {orig}  (unmapped)")
        w("")

    # --- Equations with high product term counts ---
    w("PRODUCT TERM USAGE (signals using >3 PTs)")
    w("-" * 72)
    heavy = [(name, eq, pt) for name, eq, pt in equations if pt > 3]
    heavy.sort(key=lambda x: -x[2])
    if heavy:
        for name, eq, pt in heavy:
            resolved = resolve_name(name.split('.')[0], id_map)
            suffix = name[name.index('.'):] if '.' in name else ''
            display = resolved + suffix if resolved != name.split('.')[0] else name
            w(f"  {pt:2d} PTs  {display}")
    else:
        w("  (none)")
    w("")

    # --- Equation dump (full) ---
    w("ALL EQUATIONS")
    w("-" * 72)
    for name, eq, pt in equations:
        resolved = resolve_name(name.split('.')[0], id_map)
        suffix = name[name.index('.'):] if '.' in name else ''
        display = resolved + suffix if resolved != name.split('.')[0] else name

        # Also resolve IDs within the equation body
        def replace_id(m):
            eid = m.group(0).rstrip('.Q').rstrip('.QN')
            # Strip the .Q suffix for lookup
            bare = re.sub(r'\.(Q|QN)$', '', m.group(0))
            bare_id = re.sub(r'(Q|QN)$', '', bare)
            r = id_map.get(bare_id, bare_id)
            # Preserve .Q suffix if present
            if m.group(0).endswith('.Q'):
                return r
            elif m.group(0).endswith('.QN'):
                return '!' + r
            return r

        resolved_eq = re.sub(r'id\d+(?:\.(?:Q|QN))?', replace_id, eq)
        # Also resolve XXL_ and Com_Ctrl_ if possible
        resolved_eq = re.sub(r'(XXL_\d+|Com_Ctrl_\d+)', lambda m: resolve_name(m.group(0), id_map), resolved_eq)

        w(f"  [{pt:2d} PTs]  {display} = {resolved_eq}")
    w("")

    return '\n'.join(out)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Generate readable ATF1508 fitter report from .fit and .edif files")
    ap.add_argument('fit_file', help='Path to .fit file from fit1508.exe')
    ap.add_argument('edif_file', help='Path to .edif file from yosys')
    ap.add_argument('-o', '--output', help='Output file (default: stdout)')
    args = ap.parse_args()

    id_map = parse_edif_names(args.edif_file)
    fit_data = parse_fit_file(args.fit_file)
    report = generate_report(fit_data, id_map)

    if args.output:
        with open(args.output, 'w') as f:
            f.write(report)
        print(f"Report written to {args.output}")
    else:
        print(report)


if __name__ == '__main__':
    main()
