#!/usr/bin/env python3
"""
Decode a 68000 or 68010 bus error / address error exception stack frame as
the Griffin firmware prints it (crt0.s exc_dump_frame):

    *** BUS ERROR *** (68010)
    AAAAAAAA: W0 W1 W2 ... Wn

AAAAAAAA is the SP at handler entry (optional, informational) and W0..Wn
are the frame words, 7 on a 68000 and 29 on a 68010.  The CPU is taken
from the word count unless --cpu says otherwise; a "*** ... (680x0)" header
line, if piped in with the frame, is used as a cross-check and otherwise
ignored.

68000 (7 words; MC68000UM section 5.5 "Bus Error" / "Address Error"):
    +0  Special Status Word (SSW): R/W, I/N, FC
    +2  Access address (long)
    +6  Instruction register (IR at time of fault)
    +8  Status register
    +10 PC (long) -- approximate, +/- a few words

68010 format $8 (29 words; MC68010UM section 6.3, "Bus Error and Address
Error" long format):
    +0  Status register
    +2  PC (long) -- the faulted instruction, or the next one on a prefetch
    +6  Format/vector word: 1000 | vector offset (0x8008 bus error,
        0x800C address error)
    +8  Special Status Word: RR, IF, DF, RM, HB, BY, RW, FC
    +10 Fault address (long)
    +14 unused/reserved
    +16 Data output buffer
    +18 unused/reserved
    +20 Data input buffer
    +22 unused/reserved
    +24 Instruction input buffer
    +26 Internal information, 16 words (shown raw; RTE needs them intact)
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
    # The project's container wrapper (firmware/m68k-nm) first: the unpacked
    # toolchain under firmware/m68k-unknown-elf/ is a Linux binary.
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "m68k-nm"),
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
    (?P<words>[0-9A-Fa-f]{1,4}(?:\s+[0-9A-Fa-f]{1,4})*)
    \s*$
    """,
    re.VERBOSE,
)

HEADER_RE = re.compile(r"^\s*\*\*\*.*\((?P<cpu>68000|68010)\)")

FRAME_WORDS = {"68000": 7, "68010": 29}


def decode_ssw_68010(ssw):
    bits = [
        (15, "RR", "rerun (software must rerun the cycle)"),
        (13, "IF", "instruction fetch to the instruction input buffer"),
        (12, "DF", "data fetch to the data input buffer"),
        (11, "RM", "read-modify-write cycle"),
        (10, "HB", "high byte transfer"),
        (9,  "BY", "byte transfer"),
    ]
    lines = []
    for bit, name, what in bits:
        v = (ssw >> bit) & 1
        lines.append(f"    {name} bit ({bit:2d}) = {v}  ({what})")
    rw = (ssw >> 8) & 1
    lines.append(f"    RW bit ( 8) = {rw}  ({'read' if rw else 'write'})")
    fc = ssw & 0b111
    lines.append(f"    FC  (2..0)  = {fc:03b}  ({FC_NAMES.get(fc, '?')})")
    unused = ssw & 0b0100_0000_1111_1000
    if unused:
        lines.append(f"    (reserved bits set: 0x{unused:04X})")
    return lines


def decode_frame_68000(words, frame_addr, syms):
    ssw = words[0]
    access_addr = (words[1] << 16) | words[2]
    ir = words[3]
    sr = words[4]
    pc = (words[5] << 16) | words[6]
    out = ["68000 bus/address error frame" + (f" at 0x{frame_addr:08X}:" if frame_addr is not None else ":")]
    out.append(f"  +0  SSW = 0x{ssw:04X}")
    out.extend(decode_ssw(ssw))
    out.append(f"  +2  Access address = 0x{access_addr:08X}{fmt_sym(syms, access_addr)}")
    out.append(f"  +6  Instruction reg (IR) = 0x{ir:04X}")
    out.append(f"  +8  SR = 0x{sr:04X}")
    out.extend(decode_sr(sr))
    out.append(f"  +10 PC  = 0x{pc:08X}{fmt_sym(syms, pc)}")
    out.append("          (approximate; may be a few words past the faulting instruction)")
    return out


def decode_frame_68010(words, frame_addr, syms):
    sr = words[0]
    pc = (words[1] << 16) | words[2]
    fmt = words[3]
    ssw = words[4]
    fault = (words[5] << 16) | words[6]
    dout = words[8]
    din = words[10]
    iin = words[12]
    internal = words[13:29]
    out = ["68010 format $8 frame" + (f" at 0x{frame_addr:08X}:" if frame_addr is not None else ":")]
    out.append(f"  +0  SR = 0x{sr:04X}")
    out.extend(decode_sr(sr))
    out.append(f"  +2  PC  = 0x{pc:08X}{fmt_sym(syms, pc)}")
    fmt_code = fmt >> 12
    vec_off = fmt & 0x0FFF
    kind = {0x008: "bus error (vector 2)", 0x00C: "address error (vector 3)"}.get(vec_off, "?")
    out.append(f"  +6  Format/vector = 0x{fmt:04X}  (format {fmt_code:X}, vector offset 0x{vec_off:03X}: {kind})")
    if fmt_code != 8:
        out.append("      WARNING: not format $8; this is not a 68010 bus/address error frame")
    out.append(f"  +8  SSW = 0x{ssw:04X}")
    out.extend(decode_ssw_68010(ssw))
    out.append(f"  +10 Fault address = 0x{fault:08X}{fmt_sym(syms, fault)}")
    out.append(f"  +16 Data output buffer       = 0x{dout:04X}")
    out.append(f"  +20 Data input buffer        = 0x{din:04X}")
    out.append(f"  +24 Instruction input buffer = 0x{iin:04X}")
    out.append("  +26 Internal: " + " ".join(f"{w:04X}" for w in internal))
    return out


def decode_frame(line, syms=None, cpu="auto", header_cpu=None):
    m = LINE_RE.match(line)
    if not m:
        raise ValueError(f"Could not parse stack frame line: {line!r}")
    words = [int(w, 16) for w in m.group("words").split()]
    frame_addr = int(m.group("addr"), 16) if m.group("addr") else None

    if cpu == "auto":
        by_count = {v: k for k, v in FRAME_WORDS.items()}.get(len(words))
        if by_count is None:
            raise ValueError(f"{len(words)} words: expected 7 (68000) or 29 (68010); pass --cpu to force")
        cpu = by_count
        if header_cpu and header_cpu != cpu:
            raise ValueError(f"header says {header_cpu} but the frame has {len(words)} words ({cpu})")
    elif len(words) < FRAME_WORDS[cpu]:
        raise ValueError(f"{cpu} frame needs {FRAME_WORDS[cpu]} words, got {len(words)}")

    if cpu == "68000":
        out = decode_frame_68000(words, frame_addr, syms)
    else:
        out = decode_frame_68010(words, frame_addr, syms)
    return "\n".join(out)


def main(argv):
    ap = argparse.ArgumentParser(description="Decode 68000/68010 bus and address error stack frames.")
    ap.add_argument("--elf", help="ELF file; nearest symbol will be printed for the fault address and PC.")
    ap.add_argument("--nm", help="Path to nm binary (default: auto-detect m68k-unknown-elf-nm).")
    ap.add_argument("--cpu", choices=["auto", "68000", "68010"], default="auto",
                    help="Frame layout (default: from the word count, 7 or 29).")
    ap.add_argument("frame", nargs="*",
                    help='Stack frame text, e.g. "000FFFAA: 1D75 318A EAA1 1D71 2010 00C0 13B6". '
                         "If omitted, frames are read from stdin, one per line; the firmware's "
                         '"*** BUS ERROR *** (680x0)" header lines may be included.')
    args = ap.parse_args(argv[1:])

    syms = None
    if args.elf:
        nm = find_nm(args.nm)
        if nm is None:
            print("error: could not find nm; pass --nm PATH", file=sys.stderr)
            return 2
        try:
            syms = load_symbols(args.elf, nm)
        except RuntimeError as e:
            # The container wrapper sees paths relative to firmware/.
            print(f"error: {e}", file=sys.stderr)
            return 2

    if args.frame:
        lines = [" ".join(args.frame)]
    else:
        lines = [ln for ln in sys.stdin if ln.strip() and not ln.lstrip().startswith("#")]

    first = True
    header_cpu = None
    status = 0
    for ln in lines:
        h = HEADER_RE.match(ln)
        if h:
            header_cpu = h.group("cpu")
            continue
        if not first:
            print()
        first = False
        try:
            print(decode_frame(ln, syms, args.cpu, header_cpu))
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            status = 1
        header_cpu = None
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv) or 0)
