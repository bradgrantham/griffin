#!/usr/bin/env python3
"""Wrap a flat, position-independent binary as a CP/M-68K .68K program file.

    mk68k.py INPUT.bin OUTPUT.68K

The CP/M-68K load format is a 28-byte header (magic 0x601A, text/data/bss/
symbol sizes, reserved, text base, relocation flag) followed by text, data,
symbols and, when the flag is 0, one relocation word per 16-bit word of
text+data.  A relocation word of 0 means "no fix-up", so a binary that makes
no absolute references (tools/griffin.s: PC-relative everything) gets an
all-zero map and the BDOS loader can put it anywhere in the TPA.  Anything
with absolute addresses needs real relocation entries and is not for this
tool.
"""

import struct
import sys

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(2)

with open(sys.argv[1], 'rb') as f:
    text = f.read()
if len(text) % 2:
    text += b'\0'

header = struct.pack('>HIIIIIIH', 0x601A, len(text), 0, 0, 0, 0, 0, 0)
with open(sys.argv[2], 'wb') as f:
    f.write(header)
    f.write(text)
    f.write(b'\0' * len(text))     # relocation map: no fix-ups
print(f"{sys.argv[2]}: {len(text)} bytes of text, relocatable")
