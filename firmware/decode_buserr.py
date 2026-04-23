#!/usr/bin/env python3
"""
Decode a 68000 bus error / address error exception stack frame.

Input format (one stack frame per line):
    AAAAAAAA: W0 W1 W2 W3 W4 W5 W6

Where AAAAAAAA is the address of the stack frame (optional, informational)
and W0..W6 are the seven 16-bit words pushed by the 68000 on a bus or
address error:

    +0  Special Status Word (SSW)
    +2  Access address (high word)
    +4  Access address (low word)
    +6  Instruction register (IR at time of fault)
    +8  Status register (SR at time of fault)
    +10 PC (high word) -- approximate, +/- a few words
    +12 PC (low word)

Ref: MC68000UM, Section 5.5 "Bus Error" / "Address Error".
"""

import argparse
import bisect
import os
import re
import shutil
import subprocess
import sys


FC_NAMES = {
    0b000: "(undefined/reserved 0)",
    0b001: "user data",
    0b010: "user program",
    0b011: "(undefined/reserved 3)",
    0b100: "(undefined/reserved 4)",
    0b101: "supervisor data",
    0b110: "supervisor program",
    0b111: "CPU space (int-ack)",
}


def decode_ssw(ssw):
    rw = (ssw >> 4) & 1
    in_ = (ssw >> 3) & 1
    fc = ssw & 0b111
    lines = []
    lines.append(f"    R/W bit (4) = {rw}  ({'read' if rw else 'write'})")
    lines.append(f"    I/N bit (3) = {in_} ({'not an instruction (data)' if in_ else 'instruction'})")
    lines.append(f"    FC  (2..0)  = {fc:03b}  ({FC_NAMES.get(fc, '?')})")
    unused = ssw & ~0b11111
    if unused:
        lines.append(f"    (upper bits 15..5 = 0x{unused:04X}; undefined on 68000)")
    return lines


def decode_sr(sr):
    t = (sr >> 15) & 1
    s = (sr >> 13) & 1
    ipm = (sr >> 8) & 0b111
    x = (sr >> 4) & 1
    n = (sr >> 3) & 1
    z = (sr >> 2) & 1
    v = (sr >> 1) & 1
    c = sr & 1
    flags = f"{'X' if x else 'x'}{'N' if n else 'n'}{'Z' if z else 'z'}{'V' if v else 'v'}{'C' if c else 'c'}"
    lines = []
    lines.append(f"    T (trace)       = {t}")
    lines.append(f"    S (supervisor)  = {s}  ({'supervisor' if s else 'user'} mode)")
    lines.append(f"    IPM (int mask)  = {ipm}  (mask level {ipm})")
    lines.append(f"    CCR             = {flags}  (X={x} N={n} Z={z} V={v} C={c})")
    return lines


DEFAULT_NM_CANDIDATES = [
    "m68k-unknown-elf-nm",
    "nm",
    "llvm-nm",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "m68k-unknown-elf", "bin", "m68k-unknown-elf-nm"),
]


def _runs(path):
    """Return True if the binary at `path` can actually exec on this host."""
    try:
        r = subprocess.run([path, "--version"],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        return r.returncode == 0
    except OSError:
        return False


def find_nm(explicit):
    if explicit:
        return explicit
    for cand in DEFAULT_NM_CANDIDATES:
        path = cand if os.path.isabs(cand) else shutil.which(cand)
        if not path:
            continue
        if os.path.isabs(cand) and not (os.path.isfile(path) and os.access(path, os.X_OK)):
            continue
        if _runs(path):
            return path
    return None


def load_symbols(elf_path, nm_path):
    """Return a sorted list of (addr, name) tuples from the ELF."""
    cmd = [nm_path, "-n", "--defined-only", elf_path]
    try:
        out = subprocess.check_output(cmd, text=True, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"{nm_path} failed: {e.stderr.strip()}")
    syms = []
    for ln in out.splitlines():
        parts = ln.split(None, 2)
        if len(parts) < 3:
            continue
        addr_s, type_, name = parts
        if type_ in ("a", "A", "U", "w", "W"):
            continue
        try:
            addr = int(addr_s, 16)
        except ValueError:
            continue
        syms.append((addr, name))
    syms.sort(key=lambda p: p[0])
    return syms


def nearest_symbol(syms, addr):
    if not syms:
        return None
    keys = [a for a, _ in syms]
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return None
    sym_addr, name = syms[i]
    return (name, addr - sym_addr, sym_addr)


def fmt_sym(syms, addr):
    if syms is None:
        return ""
    hit = nearest_symbol(syms, addr)
    if hit is None:
        return "  [no symbol <= addr]"
    name, off, sym_addr = hit
    return f"  [{name}+0x{off:X} @ 0x{sym_addr:08X}]"


LINE_RE = re.compile(
    r"""
    ^\s*
    (?:(?P<addr>[0-9A-Fa-f]+)\s*:\s*)?
    (?P<w0>[0-9A-Fa-f]{1,4})\s+
    (?P<w1>[0-9A-Fa-f]{1,4})\s+
    (?P<w2>[0-9A-Fa-f]{1,4})\s+
    (?P<w3>[0-9A-Fa-f]{1,4})\s+
    (?P<w4>[0-9A-Fa-f]{1,4})\s+
    (?P<w5>[0-9A-Fa-f]{1,4})\s+
    (?P<w6>[0-9A-Fa-f]{1,4})
    \s*$
    """,
    re.VERBOSE,
)


def decode_frame(line, syms=None):
    m = LINE_RE.match(line)
    if not m:
        raise ValueError(f"Could not parse stack frame line: {line!r}")
    words = [int(m.group(f"w{i}"), 16) for i in range(7)]
    ssw = words[0]
    access_addr = (words[1] << 16) | words[2]
    ir = words[3]
    sr = words[4]
    pc = (words[5] << 16) | words[6]
    frame_addr = int(m.group("addr"), 16) if m.group("addr") else None

    out = []
    if frame_addr is not None:
        out.append(f"Stack frame at 0x{frame_addr:08X}:")
    else:
        out.append("Stack frame:")
    out.append(f"  +0  SSW = 0x{ssw:04X}")
    out.extend(decode_ssw(ssw))
    out.append(f"  +2  Access address = 0x{access_addr:08X}{fmt_sym(syms, access_addr)}")
    out.append(f"  +6  Instruction reg (IR) = 0x{ir:04X}")
    out.append(f"  +8  SR = 0x{sr:04X}")
    out.extend(decode_sr(sr))
    out.append(f"  +10 PC  = 0x{pc:08X}{fmt_sym(syms, pc)}")
    out.append(f"          (approximate; may be a few words past the faulting instruction)")
    return "\n".join(out)


def main(argv):
    ap = argparse.ArgumentParser(description="Decode 68000 bus/address error stack frames.")
    ap.add_argument("--elf", help="ELF file; nearest symbol will be printed for access addr and PC.")
    ap.add_argument("--nm", help="Path to nm binary (default: auto-detect m68k-unknown-elf-nm).")
    ap.add_argument("frame", nargs="*",
                    help='Stack frame text, e.g. "000FFFAA: 1D75 318A EAA1 1D71 2010 00C0 13B6". '
                         "If omitted, frames are read from stdin, one per line.")
    args = ap.parse_args(argv[1:])

    syms = None
    if args.elf:
        nm = find_nm(args.nm)
        if nm is None:
            print("error: could not find nm; pass --nm PATH", file=sys.stderr)
            return 2
        syms = load_symbols(args.elf, nm)

    if args.frame:
        lines = [" ".join(args.frame)]
    else:
        lines = [ln for ln in sys.stdin if ln.strip() and not ln.lstrip().startswith("#")]

    first = True
    for ln in lines:
        if not first:
            print()
        first = False
        try:
            print(decode_frame(ln, syms))
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv) or 0)
