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

import re
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


def decode_frame(line):
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
    out.append(f"  +2  Access address = 0x{access_addr:08X}")
    out.append(f"  +6  Instruction reg (IR) = 0x{ir:04X}")
    out.append(f"  +8  SR = 0x{sr:04X}")
    out.extend(decode_sr(sr))
    out.append(f"  +10 PC  = 0x{pc:08X}  (approximate; may be a few words past the faulting instruction)")
    return "\n".join(out)


def main(argv):
    if len(argv) > 1:
        src = " ".join(argv[1:])
        lines = [src]
    else:
        lines = [ln for ln in sys.stdin if ln.strip() and not ln.lstrip().startswith("#")]

    first = True
    for ln in lines:
        if not first:
            print()
        first = False
        try:
            print(decode_frame(ln))
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv)
