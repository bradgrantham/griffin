#!/usr/bin/env python3
"""
kicad_netlist_summary.py — Reduce a KiCad 9 netlist to a compact, AI-friendly summary.

Parses the .net (or .xml netlist) exported from KiCad and produces:
  1. Component table (ref, value, footprint)
  2. Grouped net connections (buses collapsed, power nets separated)
  3. Potential issues (single-pin nets, unconnected pins, floating inputs)

Usage:
    python3 kicad_netlist_summary.py board.net [-o summary.txt]
    python3 kicad_netlist_summary.py board.net --section nets   # only nets
    python3 kicad_netlist_summary.py board.net --filter U1,U2   # only nets touching U1/U2
"""

import re
import sys
import argparse
from collections import defaultdict, OrderedDict


# ---------------------------------------------------------------------------
# S-expression tokenizer and parser
# ---------------------------------------------------------------------------

def tokenize_sexpr(text: str):
    """
    Tokenize a KiCad S-expression string into a list of tokens.
    Tokens are: '(', ')', or string values (quoted or unquoted).
    """
    tokens = []
    i = 0
    length = len(text)
    while i < length:
        c = text[i]
        if c in ' \t\n\r':
            i += 1
        elif c == '(':
            tokens.append('(')
            i += 1
        elif c == ')':
            tokens.append(')')
            i += 1
        elif c == '"':
            # Quoted string — scan to closing quote, handling escaped quotes
            j = i + 1
            while j < length:
                if text[j] == '\\' and j + 1 < length:
                    j += 2  # skip escaped character
                elif text[j] == '"':
                    break
                else:
                    j += 1
            tokens.append(text[i + 1:j])  # strip quotes
            i = j + 1
        else:
            # Unquoted atom — scan to whitespace or paren
            j = i
            while j < length and text[j] not in ' \t\n\r()':
                j += 1
            tokens.append(text[i:j])
            i = j
    return tokens


def parse_sexpr(tokens: list, pos: int = 0):
    """
    Parse tokens starting at pos into a nested list structure.
    Returns (parsed_node, next_pos).

    A node is either:
      - a string (atom)
      - a list of nodes (parenthesized group)
    """
    if pos >= len(tokens):
        return None, pos

    if tokens[pos] == '(':
        node = []
        pos += 1
        while pos < len(tokens) and tokens[pos] != ')':
            child, pos = parse_sexpr(tokens, pos)
            if child is not None:
                node.append(child)
        pos += 1  # skip ')'
        return node, pos
    elif tokens[pos] == ')':
        # Unexpected close paren — skip
        return None, pos + 1
    else:
        return tokens[pos], pos + 1


def parse_sexpr_all(text: str):
    """Parse the entire S-expression text and return the top-level node."""
    tokens = tokenize_sexpr(text)
    result, _ = parse_sexpr(tokens, 0)
    return result


def find_children(node: list, tag: str):
    """Find all direct child lists whose first element matches tag."""
    if not isinstance(node, list):
        return []
    return [child for child in node
            if isinstance(child, list) and len(child) > 0 and child[0] == tag]


def find_child(node: list, tag: str):
    """Find the first direct child list whose first element matches tag."""
    matches = find_children(node, tag)
    return matches[0] if matches else None


def get_field(node: list, tag: str) -> str:
    """Extract a simple (tag value) field from node. Returns '' if missing."""
    child = find_child(node, tag)
    if child and len(child) >= 2 and isinstance(child[1], str):
        return child[1]
    return ""


# ---------------------------------------------------------------------------
# Parsing KiCad netlist using S-expression tree
# ---------------------------------------------------------------------------

def parse_netlist(path: str):
    """
    Parse KiCad 9 S-expression netlist (.net file).
    Returns (components dict, nets OrderedDict).
    """
    with open(path, "r") as f:
        content = f.read()

    tree = parse_sexpr_all(content)
    if not tree or tree[0] != "export":
        print("Warning: netlist does not start with (export ...), "
              "attempting best-effort parse.", file=sys.stderr)

    # --- Components ---
    components = {}
    comp_section = find_child(tree, "components")
    if comp_section:
        for comp_node in find_children(comp_section, "comp"):
            ref = get_field(comp_node, "ref")
            if not ref:
                continue
            value = get_field(comp_node, "value")
            footprint = get_field(comp_node, "footprint")
            description = get_field(comp_node, "description")
            # Also check inside (libsource ...) for description if not at top level
            if not description:
                libsource = find_child(comp_node, "libsource")
                if libsource:
                    description = get_field(libsource, "description")
            components[ref] = {
                "value": value,
                "footprint": footprint,
                "description": description,
            }

    # --- Nets ---
    nets = OrderedDict()
    nets_section = find_child(tree, "nets")
    if nets_section:
        for net_node in find_children(nets_section, "net"):
            code = get_field(net_node, "code")
            name = get_field(net_node, "name")
            if not name:
                # Some netlists use unquoted net names as second positional element
                if len(net_node) >= 3 and isinstance(net_node[1], str):
                    name = net_node[1]
                else:
                    continue

            nodes = []
            for node_elem in find_children(net_node, "node"):
                ref = get_field(node_elem, "ref")
                pin = get_field(node_elem, "pin")
                func = get_field(node_elem, "pinfunction")
                ptype = get_field(node_elem, "pintype")
                if ref and pin:
                    nodes.append({
                        "ref": ref,
                        "pin": pin,
                        "function": func,
                        "type": ptype,
                    })
            nets[name] = nodes

    return components, nets


# ---------------------------------------------------------------------------
# Classification helpers
# ---------------------------------------------------------------------------

POWER_PATTERNS = re.compile(
    r'^(\+\d|VCC|VDD|GND|VSS|VBUS|V3\.3|3V3|5V|12V|AGND|DGND|AVCC|AVDD)',
    re.IGNORECASE
)


def is_power_net(name: str) -> bool:
    return bool(POWER_PATTERNS.match(name))


def classify_nets(nets: dict):
    """Split nets into power, signal, and bus groups."""
    power = {}
    signal = {}

    for name, nodes in nets.items():
        if name == "unconnected" or name.startswith("unconnected-"):
            continue
        if is_power_net(name):
            power[name] = nodes
        else:
            signal[name] = nodes

    return power, signal


def collapse_buses(signal_nets: dict):
    """
    Group nets that look like bus members: NAME0, NAME1 ... or NAME[0], NAME[1].
    Returns list of (group_label, member_nets) where member_nets is a list of
    (net_name, nodes).
    """
    bus_regex = re.compile(r'^(.*?)[\[_]?(\d+)\]?$')

    groups = defaultdict(list)
    standalone = []

    for name, nodes in signal_nets.items():
        m = bus_regex.match(name)
        if m:
            prefix = m.group(1).rstrip("_")
            index = int(m.group(2))
            groups[prefix].append((index, name, nodes))
        else:
            standalone.append((name, nodes))

    result = []
    used_prefixes = set()

    for prefix, members in sorted(groups.items()):
        if len(members) >= 2:
            members.sort(key=lambda x: x[0])
            indices = [m[0] for m in members]
            lo, hi = min(indices), max(indices)
            label = f"{prefix}[{lo}:{hi}]"
            result.append((label, members))
            used_prefixes.add(prefix)
        else:
            # Single member — treat as standalone
            _, name, nodes = members[0]
            standalone.append((name, nodes))

    # Sort standalone alphabetically
    standalone.sort(key=lambda x: x[0])

    return result, standalone


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------

def format_node(n: dict, show_type: bool = False) -> str:
    s = f"{n['ref']}.{n['pin']}"
    if n["function"]:
        s += f"({n['function']})"
    if show_type and n["type"]:
        s += f" [{n['type']}]"
    return s


def format_components(components: dict) -> str:
    lines = ["COMPONENTS", "=" * 70]
    # Group by prefix (U, R, C, etc.)
    groups = defaultdict(list)
    for ref, info in components.items():
        prefix = re.match(r'^([A-Z]+)', ref)
        key = prefix.group(1) if prefix else "?"
        groups[key].append((ref, info))

    for key in sorted(groups.keys()):
        members = sorted(groups[key], key=lambda x: _natural_sort_key(x[0]))
        lines.append(f"\n--- {_prefix_label(key)} ---")
        for ref, info in members:
            fp_short = info["footprint"].split(":")[-1] if info["footprint"] else ""
            line = f"  {ref:<10} {info['value']:<20}"
            if fp_short:
                line += f"  [{fp_short}]"
            lines.append(line)

    return "\n".join(lines)


def format_power_nets(power: dict) -> str:
    lines = ["\nPOWER NETS", "=" * 70]
    for name in sorted(power.keys()):
        nodes = power[name]
        refs = sorted(set(n["ref"] for n in nodes))
        lines.append(f"  {name:<16} -> {', '.join(refs)}")
    return "\n".join(lines)


def format_signal_nets(signal_nets: dict, ref_filter: set = None) -> str:
    buses, standalone = collapse_buses(signal_nets)

    lines = ["\nSIGNAL NETS", "=" * 70]

    if buses:
        lines.append("\n--- Buses ---")
        for label, members in buses:
            # Show the bus label, then a compact per-member listing
            # Check if all members connect the same set of components
            all_refs = set()
            for _, _, nodes in members:
                all_refs.update(n["ref"] for n in nodes)

            if ref_filter and not all_refs & ref_filter:
                continue

            lines.append(f"\n  {label}  ({len(members)} bits)")

            # If every member connects the same refs, show compact form
            ref_sets = [frozenset(n["ref"] for n in nodes) for _, _, nodes in members]
            if len(set(ref_sets)) == 1:
                common_refs = sorted(ref_sets[0])
                lines.append(f"    All bits: {' <-> '.join(common_refs)}")
            else:
                # Show per-bit connections
                for idx, name, nodes in members:
                    conns = ", ".join(format_node(n) for n in nodes)
                    lines.append(f"    {name}: {conns}")

    if standalone:
        lines.append("\n--- Individual signals ---")
        for name, nodes in standalone:
            refs_in_net = {n["ref"] for n in nodes}
            if ref_filter and not refs_in_net & ref_filter:
                continue
            conns = ", ".join(format_node(n, show_type=True) for n in nodes)
            lines.append(f"  {name}: {conns}")

    return "\n".join(lines)


def format_issues(nets: dict) -> str:
    lines = ["\nPOTENTIAL ISSUES", "=" * 70]
    found = False

    # Single-pin nets (often wiring errors)
    singles = [(name, nodes) for name, nodes in nets.items()
               if len(nodes) == 1
               and not name.startswith("unconnected")
               and not is_power_net(name)]
    if singles:
        found = True
        lines.append("\n--- Single-pin nets (possibly unfinished wiring) ---")
        for name, nodes in sorted(singles):
            lines.append(f"  {name}: {format_node(nodes[0])}")

    # Nets with only passive/input pins and no driver
    driver_types = {"output", "tri_state", "open_collector", "open_emitter",
                    "bidirectional", "power_out"}
    no_driver = []
    for name, nodes in nets.items():
        if is_power_net(name) or name.startswith("unconnected"):
            continue
        if len(nodes) < 2:
            continue
        types = {n["type"].lower().replace(" ", "_") for n in nodes if n["type"]}
        if types and not types & driver_types:
            no_driver.append((name, nodes, types))

    if no_driver:
        found = True
        lines.append("\n--- Nets with no apparent driver (all pins are input/passive) ---")
        for name, nodes, types in sorted(no_driver):
            conns = ", ".join(format_node(n, show_type=True) for n in nodes)
            lines.append(f"  {name}: {conns}")

    if not found:
        lines.append("  (no issues detected)")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Utility
# ---------------------------------------------------------------------------

def _natural_sort_key(s):
    return [int(c) if c.isdigit() else c.lower() for c in re.split(r'(\d+)', s)]


def _prefix_label(prefix):
    labels = {
        "U": "ICs",
        "R": "Resistors",
        "C": "Capacitors",
        "D": "Diodes",
        "Q": "Transistors",
        "J": "Connectors",
        "SW": "Switches",
        "L": "Inductors",
        "F": "Fuses",
        "Y": "Crystals / Oscillators",
        "TP": "Test Points",
        "FB": "Ferrite Beads",
        "RN": "Resistor Networks",
    }
    return labels.get(prefix, prefix)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Produce an AI-friendly summary of a KiCad 9 netlist."
    )
    parser.add_argument("netlist", help="Path to .net file")
    parser.add_argument("-o", "--output", help="Output file (default: stdout)")
    parser.add_argument(
        "--section",
        choices=["all", "components", "power", "nets", "issues"],
        default="all",
        help="Which section to output",
    )
    parser.add_argument(
        "--filter",
        help="Comma-separated ref designators to filter signal nets (e.g. U1,U2)",
    )
    args = parser.parse_args()

    components, nets = parse_netlist(args.netlist)
    power, signal = classify_nets(nets)

    ref_filter = None
    if args.filter:
        ref_filter = set(r.strip() for r in args.filter.split(","))

    sections = []
    sec = args.section

    if sec in ("all", "components"):
        sections.append(format_components(components))
    if sec in ("all", "power"):
        sections.append(format_power_nets(power))
    if sec in ("all", "nets"):
        sections.append(format_signal_nets(signal, ref_filter))
    if sec in ("all", "issues"):
        sections.append(format_issues(nets))

    output = "\n\n".join(sections) + "\n"

    if args.output:
        with open(args.output, "w") as f:
            f.write(output)
        print(f"Wrote {args.output} ({len(output)} chars)", file=sys.stderr)
    else:
        print(output)


if __name__ == "__main__":
    main()
