#!/usr/bin/env python3
"""
codegen.py — Generate C/C++, GAS assembly, GNU ld, and Verilog headers from griffin.yml.

Usage:
    python3 codegen.py griffin.yml [--outdir DIR]

Outputs (written to --outdir, default: directory of input file):
    griffin.generated.h    — C/C++ header  (namespace Griffin)
    griffin.generated.inc  — GAS assembly  (.equ directives)
    griffin.generated.ld   — GNU ld include (MEMORY origins/lengths + register symbols)
    griffin.generated.vh   — Verilog include (`define directives)
"""

import sys
import argparse
import yaml
from pathlib import Path
from datetime import date


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def parse_int(v):
    """Parse an integer or hex/binary string from YAML."""
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        return int(v, 0)
    raise ValueError(f"Cannot parse as integer: {v!r}")


def parse_bit_spec(s):
    """
    Parse a bit-spec string.  Returns (hi, lo) inclusive.
      "3"   -> (3, 3)
      "2:0" -> (2, 0)
    """
    s = str(s)
    if ':' in s:
        hi_s, lo_s = s.split(':', 1)
        return int(hi_s), int(lo_s)
    b = int(s)
    return b, b


def bit_mask(hi, lo):
    """Bitmask for bits hi:lo inclusive, positioned at the right offset."""
    width = hi - lo + 1
    return ((1 << width) - 1) << lo


def fmt_hex(v, min_digits=2):
    """Format an integer as an uppercase hex string with at least min_digits digits."""
    digits = max(min_digits, (v.bit_length() + 3) // 4)
    return f"0x{v:0{digits}X}"


BANNER = f"Generated from griffin.yml by codegen.py on {date.today()} — do not edit"


def get_dtack_ws(periph, sysclk_hz):
    """Extract the wait-state count for a peripheral at the given system clock.
    Returns None for handshake peripherals (string dtack) or missing dtack."""
    dtack = periph.get('dtack')
    if dtack is None or isinstance(dtack, str):
        return None
    for entry in dtack:
        for freq, ws in entry.items():
            if freq == sysclk_hz:
                return ws
    return None


def dtack_threshold(ws):
    """Convert wait states to Verilog ws_cnt threshold, clamped to 14 (BERR at 15)."""
    return min(2 + 2 * ws, 14)


def dtack_penalty(ws):
    """Convert wait states to emulator extra-cycle penalty (threshold minus baseline)."""
    return dtack_threshold(ws) - 2


def reg_default(reg):
    """Compute the power-on default for a register from its bits' 'default' fields.
    Returns (value, True) if any bit has a default, else (0, False)."""
    has_default = False
    val = 0
    for bf in reg.get('bits', []):
        d = bf.get('default')
        if d is not None:
            has_default = True
            _, lo = parse_bit_spec(bf['bit'])
            val |= parse_int(d) << lo
    return val, has_default


# ---------------------------------------------------------------------------
# C / C++ header
# ---------------------------------------------------------------------------

def write_c_header(hw: dict, path: Path) -> None:
    proj = hw['project']
    perifs = hw.get('peripherals', {})
    consts = hw.get('constants', {})

    lines = []
    w = lines.append

    w(f"// {BANNER}")
    w("// yaml-language-server: $schema=hw_schema.yml")
    w("")
    w("#pragma once")
    w("")
    w("#include <cstdint>")
    w("")
    w("// Memory range helper used by the emulator for address decode.")
    w("struct MemoryRange {")
    w("    uint32_t base;")
    w("    uint32_t size;")
    w("    constexpr MemoryRange(uint32_t b, uint32_t s) : base(b), size(s) {}")
    w("    constexpr bool contains(uint32_t addr) const { return addr >= base && addr < base + size; }")
    w("    constexpr uint32_t offset(uint32_t addr) const { return addr - base; }")
    w("};")
    w("")
    w("namespace Griffin {")
    w("")
    w(f"// Project: {proj['name']}")
    w(f"static constexpr uint32_t SYSCLK_HZ = {proj['clock_hz']}UL;")
    w("")

    # Track peripherals for IO span synthesis
    io_candidates = []     # (base, end) of non-linker peripherals

    for pname, periph in perifs.items():
        ar = periph.get('address_range')
        if not ar:
            continue

        base = parse_int(ar['base'])
        size = parse_int(ar.get('size', 0))
        window = parse_int(ar['window']) if ar.get('window') else size
        linker_section = (periph.get('linker') or {}).get('section')
        desc = periph.get('description') or periph.get('part') or ''

        w(f"// {'-' * 60}")
        w(f"// {pname}" + (f": {desc}" if desc else ""))
        w(f"static constexpr uint32_t {pname}_BASE = {fmt_hex(base, 6)}UL;")
        if size:
            w(f"static constexpr uint32_t {pname}_SIZE = {fmt_hex(size, 6)}UL;")
        if window and window != size:
            w(f"static constexpr uint32_t {pname}_WINDOW = {fmt_hex(window, 6)}UL;")

        # MemoryRange spans the full decode window so .contains() is correct for address decode.
        if window:
            w(f"inline constexpr MemoryRange {pname}({fmt_hex(base, 6)}UL, {fmt_hex(window, 6)}UL);")

        # Interrupt level
        intr = periph.get('interrupt')
        if intr and isinstance(intr, dict) and 'level' in intr:
            w(f"static constexpr uint32_t {pname}_IRQ_LEVEL = {intr['level']}U;")

        # Peripheral clock
        clk = periph.get('clock')
        if clk is not None:
            w(f"static constexpr uint32_t {pname}_CLOCK = {parse_int(clk)}UL;")

        # DTACK wait-state constants (for peripherals with fixed wait states)
        ws = get_dtack_ws(periph, proj['clock_hz'])
        if ws is not None:
            thresh = dtack_threshold(ws)
            pen = dtack_penalty(ws)
            w(f"static constexpr int {pname}_DTACK_WS = {ws};  // wait states at {proj['clock_hz']} Hz")
            w(f"static constexpr int {pname}_DTACK_THRESHOLD = {thresh};  // ws_cnt threshold for Verilog")
            w(f"static constexpr int {pname}_DTACK_PENALTY = {pen};  // extra SYSCLK cycles for emulator")

        # IO span: only peripherals in the 0xF0_0000 IO area (not VIDEO/ENGINE/ROM/RAM).
        if linker_section is None and base >= 0xF00000:
            if window:
                io_candidates.append((base, base + window))

        # Registers
        for reg in periph.get('registers', []):
            offset = parse_int(reg['offset'])
            rname  = reg['name']
            access = reg.get('access', 'rw')
            rdesc  = reg.get('description', '')
            addr   = base + offset

            comment = f"  // {access.upper()}"
            if rdesc:
                # Trim long descriptions to one sentence for the comment
                short = rdesc.split('.')[0].strip()
                comment += f": {short}"
            w(f"static constexpr uint32_t {pname}_{rname} = {fmt_hex(addr, 6)}UL;{comment}")

            for bf in reg.get('bits', []):
                hi, lo  = parse_bit_spec(bf['bit'])
                bname   = bf['name']
                mask    = bit_mask(hi, lo)
                prefix  = f"{pname}_{rname}" if (rname == bname or rname.endswith('_' + bname)) else f"{pname}_{rname}_{bname}"
                w(f"static constexpr uint32_t {prefix}_MASK  = {fmt_hex(mask)}U;  // bits {hi}:{lo}")
                w(f"static constexpr uint32_t {prefix}_SHIFT = {lo}U;")

                for ev in bf.get('values', []):
                    vname = ev['name']
                    vraw  = parse_int(ev['value'])
                    vdesc = ev.get('description', '')
                    comment = f"  // {vdesc}" if vdesc else ""
                    w(f"static constexpr uint32_t {prefix}_{vname} = {vraw}U;{comment}")

            dval, has_def = reg_default(reg)
            if has_def:
                w(f"static constexpr uint32_t {pname}_{rname}_DEFAULT = {fmt_hex(dval)}U;")

        w("")

    # Synthesize IO_BASE / IO_SIZE from peripherals with no linker section
    # that live above the RAM region.
    if io_candidates:
        io_min = min(b for b, _ in io_candidates)
        io_max = max(e for _, e in io_candidates)
        w(f"// IO region — span of all non-RAM/ROM memory-mapped peripherals")
        w(f"static constexpr uint32_t IO_BASE = {fmt_hex(io_min, 6)}UL;")
        w(f"static constexpr uint32_t IO_SIZE = {fmt_hex(io_max - io_min, 6)}UL;")
        w("")

    # Top-level constants
    if consts:
        w("// Constants")
        for cname, cval in consts.items():
            v = parse_int(cval)
            w(f"static constexpr uint32_t {cname} = {fmt_hex(v)}U;")
        w("")

    w("} // namespace Griffin")

    path.write_text('\n'.join(lines) + '\n')
    print(f"  wrote {path}")


# ---------------------------------------------------------------------------
# GAS assembly include
# ---------------------------------------------------------------------------

def write_asm_include(hw: dict, path: Path) -> None:
    proj   = hw['project']
    perifs = hw.get('peripherals', {})
    consts = hw.get('constants', {})

    lines = []
    w = lines.append

    w(f"| {BANNER}")
    w("")
    w(f"| Project: {proj['name']}")
    w(f".equ SYSCLK_HZ, {proj['clock_hz']}")
    w("")

    for pname, periph in perifs.items():
        ar = periph.get('address_range')
        if not ar:
            continue

        base = parse_int(ar['base'])
        size = parse_int(ar.get('size', 0))
        window = parse_int(ar['window']) if ar.get('window') else size
        desc = periph.get('description') or periph.get('part') or ''

        w(f"| {pname}" + (f": {desc}" if desc else ""))
        w(f".equ {pname}_BASE, {fmt_hex(base, 6)}")
        if size:
            w(f".equ {pname}_SIZE, {fmt_hex(size, 6)}")
        if window and window != size:
            w(f".equ {pname}_WINDOW, {fmt_hex(window, 6)}")

        # Interrupt level
        intr = periph.get('interrupt')
        if intr and isinstance(intr, dict) and 'level' in intr:
            w(f".equ {pname}_IRQ_LEVEL, {intr['level']}")

        # Peripheral clock
        clk = periph.get('clock')
        if clk is not None:
            w(f".equ {pname}_CLOCK, {parse_int(clk)}")

        for reg in periph.get('registers', []):
            offset = parse_int(reg['offset'])
            rname  = reg['name']
            addr   = base + offset
            access = reg.get('access', 'rw')
            w(f".equ {pname}_{rname}, {fmt_hex(addr, 6)}  | {access.upper()}")

            for bf in reg.get('bits', []):
                hi, lo = parse_bit_spec(bf['bit'])
                bname  = bf['name']
                mask   = bit_mask(hi, lo)
                prefix = f"{pname}_{rname}" if (rname == bname or rname.endswith('_' + bname)) else f"{pname}_{rname}_{bname}"
                w(f".equ {prefix}_MASK,  {fmt_hex(mask)}")
                w(f".equ {prefix}_SHIFT, {lo}")

                for ev in bf.get('values', []):
                    vname = ev['name']
                    vraw  = parse_int(ev['value'])
                    w(f".equ {prefix}_{vname}, {vraw}")

            dval, has_def = reg_default(reg)
            if has_def:
                w(f".equ {pname}_{rname}_DEFAULT, {fmt_hex(dval)}")

        w("")

    if consts:
        w("| Constants")
        for cname, cval in consts.items():
            w(f".equ {cname}, {fmt_hex(parse_int(cval))}")
        w("")

    path.write_text('\n'.join(lines) + '\n')
    print(f"  wrote {path}")


# ---------------------------------------------------------------------------
# GNU ld include
# ---------------------------------------------------------------------------

def write_ld_include(hw: dict, path: Path) -> None:
    perifs = hw.get('peripherals', {})
    consts = hw.get('constants', {})

    lines = []
    w = lines.append

    w(f"/* {BANNER} */")
    w("")

    # Memory region parameters consumed by the MEMORY block in linker.ld
    w("/* Memory region origins and lengths for MEMORY { } */")
    for pname, periph in perifs.items():
        ar = periph.get('address_range')
        if not ar:
            continue
        linker = periph.get('linker') or {}
        section = linker.get('section')
        if not section:
            continue
        base = parse_int(ar['base'])
        size = parse_int(ar.get('size', 0))
        sname = section.upper()
        w(f"{sname}_ORIGIN = {fmt_hex(base, 6)};")
        if size:
            w(f"{sname}_LENGTH = {fmt_hex(size, 6)};")

    w("")

    # Register absolute addresses as linker symbols (useful for MAP files / debuggers)
    w("/* Register absolute addresses */")
    for pname, periph in perifs.items():
        ar = periph.get('address_range')
        if not ar:
            continue
        base = parse_int(ar['base'])
        for reg in periph.get('registers', []):
            offset = parse_int(reg['offset'])
            rname  = reg['name']
            addr   = base + offset
            w(f"{pname}_{rname} = {fmt_hex(addr, 6)};")
            dval, has_def = reg_default(reg)
            if has_def:
                w(f"{pname}_{rname}_DEFAULT = {fmt_hex(dval)};")

    w("")

    if consts:
        w("/* Constants */")
        for cname, cval in consts.items():
            w(f"{cname} = {fmt_hex(parse_int(cval))};")
        w("")

    path.write_text('\n'.join(lines) + '\n')
    print(f"  wrote {path}")


# ---------------------------------------------------------------------------
# Verilog include
# ---------------------------------------------------------------------------

def write_verilog_include(hw: dict, path: Path) -> None:
    proj   = hw['project']
    perifs = hw.get('peripherals', {})
    consts = hw.get('constants', {})

    lines = []
    w = lines.append

    w(f"// {BANNER}")
    w(f"// Include with: `include \"griffin.generated.vh\"")
    w("")
    w(f"// Project: {proj['name']}")
    w(f"`define SYSCLK_HZ {proj['clock_hz']}")
    w("")

    for pname, periph in perifs.items():
        ar = periph.get('address_range')
        if not ar:
            continue

        base = parse_int(ar['base'])
        size = parse_int(ar.get('size', 0))
        window = parse_int(ar['window']) if ar.get('window') else size
        desc = periph.get('description') or periph.get('part') or ''

        w(f"// {pname}" + (f": {desc}" if desc else ""))
        w(f"`define {pname}_BASE 24'h{base:06X}")
        if size:
            w(f"`define {pname}_SIZE 24'h{size:06X}")
        if window and window != size:
            w(f"`define {pname}_WINDOW 24'h{window:06X}")

        # Interrupt level
        intr = periph.get('interrupt')
        if intr and isinstance(intr, dict) and 'level' in intr:
            w(f"`define {pname}_IRQ_LEVEL {intr['level']}")

        # Peripheral clock
        clk = periph.get('clock')
        if clk is not None:
            w(f"`define {pname}_CLOCK {parse_int(clk)}")

        # DTACK wait-state threshold (for peripherals with fixed wait states)
        ws = get_dtack_ws(periph, proj['clock_hz'])
        if ws is not None:
            thresh = dtack_threshold(ws)
            w(f"`define {pname}_DTACK_THRESHOLD 4'd{thresh}  // {ws} WS at {proj['clock_hz']} Hz")

        for reg in periph.get('registers', []):
            offset = parse_int(reg['offset'])
            rname  = reg['name']
            addr   = base + offset
            access = reg.get('access', 'rw')
            w(f"`define {pname}_{rname} 24'h{addr:06X}  // {access.upper()}")

            for bf in reg.get('bits', []):
                hi, lo = parse_bit_spec(bf['bit'])
                bname  = bf['name']
                mask   = bit_mask(hi, lo)
                width  = hi - lo + 1
                prefix = f"{pname}_{rname}" if (rname == bname or rname.endswith('_' + bname)) else f"{pname}_{rname}_{bname}"
                w(f"`define {prefix}_MASK  {width}'h{mask:02X}")
                w(f"`define {prefix}_SHIFT {lo}")

                for ev in bf.get('values', []):
                    vname = ev['name']
                    vraw  = parse_int(ev['value'])
                    w(f"`define {prefix}_{vname} {vraw}")

            dval, has_def = reg_default(reg)
            if has_def:
                reg_width = parse_int(reg.get('width', 8))
                w(f"`define {pname}_{rname}_DEFAULT {reg_width}'h{dval:02X}")

        w("")

    if consts:
        w("// Constants")
        for cname, cval in consts.items():
            v = parse_int(cval)
            w(f"`define {cname} 8'h{v:02X}")
        w("")

    path.write_text('\n'.join(lines) + '\n')
    print(f"  wrote {path}")


# ---------------------------------------------------------------------------
# MCS-51 (SDCC) C header — register offsets and constants for IO MCU firmware
# ---------------------------------------------------------------------------

def write_mcs51_header(hw: dict, path: Path) -> None:
    """Emit a C89-compatible header for SDCC with register offsets (not
    absolute addresses) and all constants.  Only peripherals that declare
    ``mcs51: true`` get their registers emitted as offsets."""
    perifs = hw.get('peripherals', {})
    consts = hw.get('constants', {})

    lines = []
    w = lines.append

    w(f"/* {BANNER} */")
    w("")
    w("#ifndef GRIFFIN_MCS51_H")
    w("#define GRIFFIN_MCS51_H")
    w("")

    for pname, periph in perifs.items():
        if not periph.get('mcs51'):
            continue
        ar = periph.get('address_range')
        if not ar:
            continue

        desc = periph.get('description') or periph.get('part') or ''
        w(f"/* {pname}: {desc} */")
        w("")

        # The MCU sees address lines A1-A4 on port pins, so the register
        # address the MCU matches against is the byte offset >> 1.
        for reg in periph.get('registers', []):
            offset = parse_int(reg['offset'])
            mcu_addr = offset >> 1
            rname  = reg['name']
            access = reg.get('access', 'rw')
            rdesc  = reg.get('description', '')
            short  = rdesc.split('.')[0].strip() if rdesc else ''
            comment = f"  /* {access.upper()}: {short} */" if short else f"  /* {access.upper()} */"
            w(f"#define {pname}_REG_{rname}  {fmt_hex(mcu_addr)}{comment}")

            for bf in reg.get('bits', []):
                hi, lo = parse_bit_spec(bf['bit'])
                bname  = bf['name']
                mask   = bit_mask(hi, lo)
                prefix = f"{pname}_{rname}" if (rname == bname or rname.endswith('_' + bname)) else f"{pname}_{rname}_{bname}"
                w(f"#define {prefix}_MASK   {fmt_hex(mask)}")
                w(f"#define {prefix}_SHIFT  {lo}")

                for ev in bf.get('values', []):
                    vname = ev['name']
                    vraw  = parse_int(ev['value'])
                    vdesc = ev.get('description', '')
                    comment = f"  /* {vdesc} */" if vdesc else ""
                    w(f"#define {prefix}_{vname}  {vraw}{comment}")

            dval, has_def = reg_default(reg)
            if has_def:
                w(f"#define {pname}_{rname}_DEFAULT  {fmt_hex(dval)}")

        w("")

    if consts:
        w("/* Constants */")
        for cname, cval in consts.items():
            v = parse_int(cval)
            w(f"#define {cname}  {fmt_hex(v)}")
        w("")

    w("#endif /* GRIFFIN_MCS51_H */")

    path.write_text('\n'.join(lines) + '\n')
    print(f"  wrote {path}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input', help='Path to griffin.yml')
    ap.add_argument('--outdir', default=None,
                    help='Output directory (default: directory of input file)')
    args = ap.parse_args()

    src = Path(args.input)
    if not src.exists():
        print(f"error: {src} not found", file=sys.stderr)
        sys.exit(1)

    outdir = Path(args.outdir) if args.outdir else src.parent
    outdir.mkdir(parents=True, exist_ok=True)

    with src.open() as f:
        hw = yaml.safe_load(f)

    stem = src.stem  # "griffin"
    print(f"Generating from {src} → {outdir}/")
    write_c_header(       hw, outdir / f"{stem}.generated.h")
    write_asm_include(    hw, outdir / f"{stem}.generated.inc")
    write_ld_include(     hw, outdir / f"{stem}.generated.ld")
    write_verilog_include(hw, outdir / f"{stem}.generated.vh")
    write_mcs51_header(   hw, outdir / f"{stem}.generated.mcs51.h")


if __name__ == '__main__':
    main()
