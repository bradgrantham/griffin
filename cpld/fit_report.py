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

def resolve_name(signal, id_map, annotations=None):
    """Resolve a fitter signal name to a Verilog name using the EDIF map.
    If annotations is provided, append heuristic label for abc: nodes."""
    # Strip suffixes like Q, QN for lookup
    bare = re.sub(r'(Q|QN)$', '', signal)
    if bare in id_map:
        suffix = signal[len(bare):]
        name = id_map[bare]
        if suffix == 'QN' and '(QN)' not in name:
            name = name + " (inv)"
        if annotations and name.startswith('abc:') or (annotations and '(QN)' in name and name.replace(' (QN)', '').replace(' (inv)', '').startswith('abc:')):
            base = name.replace(' (QN)', '').replace(' (inv)', '')
            if base in annotations:
                name = name + '  «' + annotations[base] + '»'
        return name
    # XXL_ and Com_Ctrl_ are yosys-internal; just return as-is
    return signal


def resolve_signals(signals, id_map, annotations=None):
    """Resolve a list of signals, returning list of (original, resolved) tuples."""
    return [(s, resolve_name(s, id_map, annotations)) for s in signals]


# ---------------------------------------------------------------------------
# Heuristic annotation of ABC optimizer nodes
# ---------------------------------------------------------------------------

# Signals that form the bus-cycle / address-decode vocabulary
ADDR_SIGNALS = {
    'A_hi_0', 'A_hi_1', 'A_hi_2', 'A_hi_3', 'A_hi_4', 'A_hi_5',
    'A_lo_0', 'A_lo_1', 'A_lo_2', 'A_lo_3', 'A_lo_4',
    'nAS', 'nLDS', 'nUDS', 'R_nW', 'FC_0', 'FC_1', 'FC_2',
}

SYSTICK_SIGNALS = re.compile(r'systick_')
TIMER_SIGNALS = re.compile(r'timer_')
WS_SIGNALS = re.compile(r'ws_cnt_')
PRESCALE_SIGNALS = re.compile(r'timer_prescale_')


def _extract_refs(eq_text):
    """Extract all signal names referenced in an equation string."""
    # Match word-like tokens including abc:nXXX forms, strip .Q/.QN/.PIN suffixes
    refs = set()
    for tok in re.findall(r'[A-Za-z_]\w*(?::\w+)?(?:\.\w+)?', eq_text):
        base = re.sub(r'\.(Q|QN|PIN)$', '', tok)
        refs.add(base)
    return refs


def _decode_address_from_pt(pt_text):
    """Try to decode an address from a single product term's A_hi/A_lo bits.
    Returns (addr_hi, addr_lo, has_lo) or None if not enough address bits."""
    hi_bits = {}  # bit_num → 0 or 1
    lo_bits = {}
    for m in re.finditer(r'(!?)A_hi_(\d)', pt_text):
        hi_bits[int(m.group(2))] = 0 if m.group(1) == '!' else 1
    for m in re.finditer(r'(!?)A_lo_(\d)', pt_text):
        lo_bits[int(m.group(2))] = 0 if m.group(1) == '!' else 1

    if len(hi_bits) < 3:
        return None

    # A_hi maps to A23:A18 (hi_5=A23, hi_0=A18)
    addr_hi = 0
    for bit, val in hi_bits.items():
        addr_hi |= val << (bit + 18)

    addr_lo = 0
    has_lo = len(lo_bits) > 0
    for bit, val in lo_bits.items():
        # A_lo maps to A4:A0 (lo_4=A4, lo_0=A0) — register offset bits
        addr_lo |= val << (bit + 1)  # byte addresses: A_lo_0 is A1

    return addr_hi, addr_lo, has_lo


def _describe_address(addr_hi, addr_lo, has_lo, is_read, is_write):
    """Produce a short human label for a decoded address."""
    # Known peripheral ranges from griffin.yml
    peripherals = [
        (0x000000, 0x100000, "RAM1"),
        (0x100000, 0x100000, "RAM2"),
        (0x200000, 0x100000, "RAM3"),
        (0x300000, 0x100000, "RAM4"),
        (0xC00000, 0x100000, "ROM"),
        (0xD00000, 0x100000, "ENGINE"),
        (0xE00000, 0x100000, "VIDEO"),
        (0xF00000, 0x040000, "GLUE"),
        (0xF40000, 0x040000, "CF"),
        (0xFC0000, 0x040000, "AUDIO"),
    ]

    periph = None
    for base, size, name in peripherals:
        if base <= addr_hi < base + size:
            periph = name
            break

    rw = ""
    if is_read and not is_write:
        rw = " RD"
    elif is_write and not is_read:
        rw = " WR"

    if periph == "GLUE" and has_lo:
        # GLUE is 8-bit on low data byte (nLDS), so A_lo bits give even
        # byte addresses; actual register offsets are odd (addr | 1)
        glue_regs = {
            0x00: "DEBUG",     # reg offset 0x01
            0x02: "SYSTICK",   # reg offset 0x03
            0x06: "CONFIG",    # reg offset 0x07
            0x08: "TIMER",     # reg offset 0x09
            0x0A: "TIMER_ARM", # reg offset 0x0B
        }
        offset = addr_lo & 0x1F
        reg = glue_regs.get(offset, f"@0x{offset|1:02X}")
        return f"GLUE {reg}{rw} decode"

    if periph:
        if has_lo:
            return f"{periph} @0x{addr_lo:02X}{rw} decode"
        return f"{periph} select{rw}"

    return f"addr 0x{addr_hi:06X}{rw} decode"


def annotate_abc_nodes(equations, id_map):
    """Generate heuristic labels for abc:nXXX nodes based on equation analysis.

    Returns dict: 'abc:nXXX' → short descriptive string.
    """
    annotations = {}

    # Build resolved equation lookup: resolved_name → (eq_text, pt_count)
    eq_by_resolved = {}
    for name, eq, pt in equations:
        rname = resolve_name(name.split('.')[0], id_map)
        suffix = name[name.index('.'):] if '.' in name else ''
        eq_by_resolved[rname + suffix] = (eq, pt)
        eq_by_resolved[rname] = (eq, pt)

    # Also build raw-name lookup
    eq_by_raw = {name: (eq, pt) for name, eq, pt in equations}

    # Pre-resolve all equations for reference analysis
    def _resolve_eq_for_refs(eq_text):
        """Resolve fitter IDs in equation text to mapped names for analysis."""
        def replace_id(m):
            bare = re.sub(r'(Q|QN)$', '', m.group(0))
            r = id_map.get(bare, m.group(0))
            return r.replace(' (QN)', '')
        return re.sub(r'id\d+(?:Q|QN)?', replace_id, eq_text)

    for name, eq, pt in equations:
        rname = resolve_name(name.split('.')[0], id_map)
        if not rname.startswith('abc:'):
            continue
        # Strip (QN) for annotation key
        base = rname.replace(' (QN)', '')
        if base in annotations:
            continue

        resolved_eq = _resolve_eq_for_refs(eq)
        refs = _extract_refs(resolved_eq)

        # --- Address decode detection ---
        non_addr = refs - ADDR_SIGNALS - {''}
        # Allow abc:nXXX refs, id refs, and rom_overlay_disable in address terms
        non_addr_non_abc = {s for s in non_addr
                           if not s.startswith('abc') and not s.startswith('id')
                           and s not in ('rom_overlay_disable', 'nreset_sync2',
                                         'PIN', 'Q', 'QN')}
        is_addr_decode = len(non_addr_non_abc) == 0 and len(refs & ADDR_SIGNALS) >= 3

        if is_addr_decode:
            # Decode from first product term (use resolved_eq for the terms)
            first_pt = resolved_eq.split('#')[0] if '#' in resolved_eq else resolved_eq
            decoded = _decode_address_from_pt(first_pt)
            is_read = 'R_nW' in refs and '!R_nW' not in first_pt
            is_write = '!R_nW' in first_pt
            if decoded:
                addr_hi, addr_lo, has_lo = decoded
                annotations[base] = _describe_address(
                    addr_hi, addr_lo, has_lo, is_read, is_write)
            else:
                # Partial decode — describe what we can
                hi_bits = {}
                for m in re.finditer(r'(!?)A_hi_(\d)', first_pt):
                    hi_bits[int(m.group(2))] = 0 if m.group(1) == '!' else 1
                if hi_bits:
                    addr = sum(v << (b + 18) for b, v in hi_bits.items())
                    # Try to describe the range
                    desc = _describe_address(addr, 0, False, is_read, is_write)
                    if desc.startswith("addr"):
                        annotations[base] = f"addr 0x{addr:06X} partial decode"
                    else:
                        annotations[base] = desc
                else:
                    annotations[base] = "addr decode"
            continue

        # --- Signal family classification ---
        systick_refs = {s for s in refs if SYSTICK_SIGNALS.match(s)}
        timer_refs = {s for s in refs if TIMER_SIGNALS.match(s)}
        ws_refs = {s for s in refs if WS_SIGNALS.match(s)}
        prescale_refs = {s for s in refs if PRESCALE_SIGNALS.match(s)}

        # Prescaler bits are shared between systick chain and timer
        # so count them toward whichever has more other refs
        if systick_refs and not timer_refs - prescale_refs:
            systick_refs |= prescale_refs
        elif timer_refs and not systick_refs:
            timer_refs |= prescale_refs

        total = len(refs)
        if total == 0:
            continue

        # Check consumers to refine the label
        raw_name_base = name.split('.')[0]
        consumers_systick = 0
        consumers_timer = 0
        consumers_dtack = 0
        for cn, ceq, cpt in equations:
            if raw_name_base in ceq:
                crname = resolve_name(cn.split('.')[0], id_map)
                if 'systick' in crname or 'systick' in cn:
                    consumers_systick += 1
                if 'timer' in crname or 'timer' in cn:
                    consumers_timer += 1
                if 'DTACK' in crname or 'DTACK' in cn:
                    consumers_dtack += 1

        # Mostly systick signals?
        if len(systick_refs) >= 4 and len(systick_refs) > len(timer_refs - prescale_refs):
            if all(('subdiv' in s or 'prescale' in s) for s in systick_refs | prescale_refs if s in refs):
                annotations[base] = "systick prescale+subdiv zero"
            elif any('cnt' in s for s in systick_refs):
                cnt_refs = {s for s in systick_refs if 'cnt' in s}
                subdiv_refs = {s for s in systick_refs if 'subdiv' in s}
                if subdiv_refs and cnt_refs:
                    annotations[base] = "systick reload detect"
                elif cnt_refs:
                    annotations[base] = "systick cnt partial"
                else:
                    annotations[base] = "systick chain"
            else:
                annotations[base] = "systick chain"
            continue

        # Mostly timer signals?
        if len(timer_refs) >= 3 and len(timer_refs) > len(systick_refs):
            period_refs = {s for s in timer_refs if 'period' in s}
            cnt_refs = {s for s in timer_refs if 'cnt' in s}
            if period_refs and not cnt_refs:
                annotations[base] = "timer period nonzero"
            elif cnt_refs and not period_refs:
                annotations[base] = "timer cnt partial"
            elif period_refs and cnt_refs:
                annotations[base] = "timer cnt!=period"
            elif prescale_refs and not period_refs and not cnt_refs:
                annotations[base] = "timer prescale zero"
            else:
                annotations[base] = "timer logic"
            continue

        # Wait-state related?
        if ws_refs and len(ws_refs) >= len(refs) / 2:
            annotations[base] = "wait-state counter"
            continue

        # Mixed: DTACK / wait-state / bus logic
        if consumers_dtack > 0 and (ws_refs or 'rom_overlay_disable' in refs):
            annotations[base] = "DTACK generation"
            continue

        # Has address decode bits and feeds selects?
        if refs & {'nAS', 'A_hi_3', 'A_hi_4', 'A_hi_5'}:
            if consumers_dtack:
                annotations[base] = "bus cycle decode (DTACK path)"
            else:
                annotations[base] = "bus cycle decode"
            continue

        # Fallback: describe by dominant consumer
        if consumers_systick > consumers_timer and consumers_systick > 0:
            annotations[base] = "systick gate"
        elif consumers_timer > 0:
            annotations[base] = "timer gate"

    # --- Second pass: annotate abc nodes that appear only as references ---
    # These have no equation of their own in the .fit file but are referenced
    # in other equations.  Classify by consumer families.
    all_abc_refs = set()
    for name, eq, pt in equations:
        for m in re.finditer(r'(abc:n\d+)', resolve_name(name.split('.')[0], id_map)):
            all_abc_refs.add(m.group(1))
        # Also scan equation text for id→abc mappings
        for m in re.finditer(r'id\d+', eq):
            bare = re.sub(r'(Q|QN)$', '', m.group(0))
            mapped = id_map.get(bare, '')
            if mapped.startswith('abc:'):
                all_abc_refs.add(mapped.replace(' (QN)', ''))

    for name, eq, pt in equations:
        eq_resolved = eq
        for m_id in re.finditer(r'id\d+', eq):
            bare = re.sub(r'(Q|QN)$', '', m_id.group(0))
            mapped = id_map.get(bare, '')
            if mapped.startswith('abc:'):
                base = mapped.replace(' (QN)', '')
                all_abc_refs.add(base)

    for abc_name in all_abc_refs:
        if abc_name in annotations:
            continue

        # Find all equations that reference this abc node
        # We need to search for the raw fitter ID
        raw_ids = []
        for fid, mapped in id_map.items():
            base = mapped.replace(' (QN)', '')
            if base == abc_name:
                raw_ids.append(fid)

        if not raw_ids:
            continue

        consumers_systick = 0
        consumers_timer = 0
        consumers_dtack = 0
        consumers_subdiv = 0
        consumer_names = []
        for eq_name, eq_text, pt in equations:
            used = False
            for rid in raw_ids:
                if rid in eq_text:
                    used = True
                    break
            if not used:
                continue
            rn = resolve_name(eq_name.split('.')[0], id_map)
            consumer_names.append(rn)
            if 'systick_subdiv' in eq_name or 'systick_subdiv' in rn:
                consumers_subdiv += 1
            if 'systick' in eq_name or 'systick' in rn:
                consumers_systick += 1
            if 'timer' in eq_name or 'timer' in rn:
                consumers_timer += 1
            if 'DTACK' in eq_name or 'DTACK' in rn:
                consumers_dtack += 1

        # Check for address-decode / bus-cycle consumers
        consumers_addr = 0
        for eq_name, eq_text, pt in equations:
            used = False
            for rid in raw_ids:
                if rid in eq_text:
                    used = True
                    break
            if not used:
                continue
            rn = resolve_name(eq_name.split('.')[0], id_map)
            # Check both the resolved name and its annotation
            rn_base = rn.replace(' (QN)', '')
            ann_text = annotations.get(rn_base, '')
            combined = rn + ' ' + ann_text
            if any(x in combined for x in ('select', 'SELECT', 'DTACK', 'addr', 'GLUE', 'decode', 'gate', 'generation')):
                consumers_addr += 1

        if consumers_systick == 0 and consumers_timer == 0 and consumers_dtack == 0 and consumers_addr == 0:
            continue

        if consumers_dtack and consumers_timer:
            annotations[abc_name] = "timer zero (DTACK+reload)"
        elif consumers_dtack:
            annotations[abc_name] = "DTACK gate"
        elif consumers_subdiv > consumers_systick / 2 and consumers_subdiv > 0:
            annotations[abc_name] = "systick subdiv gate"
        elif consumers_systick > consumers_timer:
            if consumers_systick >= 6:
                annotations[abc_name] = "systick tick gate"
            else:
                annotations[abc_name] = "systick partial"
        elif consumers_timer > consumers_systick:
            annotations[abc_name] = "timer partial"
        elif consumers_addr > 0:
            annotations[abc_name] = "bus cycle gate"

    return annotations


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

    # Compute heuristic annotations for abc: nodes
    annotations = annotate_abc_nodes(equations, id_map)

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
        name = resolve_name(signal, id_map, annotations)
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
            name = resolve_name(signal, id_map, annotations)
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
                    eq_resolved = resolve_name(eq_name.split('.')[0], id_map, annotations)
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
            name = resolve_name(signal, id_map, annotations)
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
            resolved = resolve_signals(signals, id_map, annotations)
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
            resolved = resolve_name(name.split('.')[0], id_map, annotations)
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
        resolved = resolve_name(name.split('.')[0], id_map, annotations)
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
