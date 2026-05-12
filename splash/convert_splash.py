#!/usr/bin/env python3
"""Convert splash_mono.pgm (8-bit grayscale) to C++ uint16_t array, LSB-first, 1=white."""

import struct, sys, os

def read_pgm(path):
    with open(path, 'rb') as f:
        magic = f.readline().strip()
        if magic not in (b'P5', b'P2'):
            raise ValueError(f"Not a PGM file: {magic}")
        # skip comments
        line = f.readline()
        while line.startswith(b'#'):
            line = f.readline()
        width, height = map(int, line.split())
        maxval = int(f.readline().strip())
        if magic == b'P5':
            data = f.read(width * height)
        else:
            data = bytes(int(x) for x in f.read().split())
    return width, height, maxval, data

def main():
    pgm_path = os.path.join(os.path.dirname(__file__) or '.', 'splash_mono.pgm')
    width, height, maxval, pixels = read_pgm(pgm_path)
    print(f"Read {pgm_path}: {width}x{height}, maxval={maxval}")

    if width != 640 or height != 240:
        print(f"WARNING: expected 640x240, got {width}x{height}", file=sys.stderr)

    threshold = maxval // 2
    words_per_row = width // 16
    total_words = words_per_row * height
    words = []

    for y in range(height):
        for wx in range(words_per_row):
            word = 0
            for bit in range(16):
                px = y * width + wx * 16 + bit
                if pixels[px] > threshold:
                    word |= (1 << (15 - bit))
            words.append(word)

    assert len(words) == total_words

    # Write header
    base = os.path.dirname(__file__) or '.'
    h_path = os.path.join(base, 'splash.h')
    with open(h_path, 'w') as f:
        f.write('#pragma once\n')
        f.write('#include <cstdint>\n\n')
        f.write(f'static constexpr int SPLASH_WIDTH  = {width};\n')
        f.write(f'static constexpr int SPLASH_HEIGHT = {height};\n')
        f.write(f'static constexpr int SPLASH_WORDS  = {total_words};\n\n')
        f.write('extern const uint16_t splash_bitmap[SPLASH_WORDS];\n')
    print(f"Wrote {h_path}")

    # Write source
    cpp_path = os.path.join(base, 'splash.cpp')
    with open(cpp_path, 'w') as f:
        f.write('#include "splash.h"\n\n')
        f.write('const uint16_t splash_bitmap[SPLASH_WORDS] = {\n')
        for i in range(0, len(words), 8):
            chunk = words[i:i+8]
            line = ', '.join(f'0x{w:04x}' for w in chunk)
            f.write(f'    {line},\n')
        f.write('};\n')
    print(f"Wrote {cpp_path} ({total_words} words, {total_words * 2} bytes)")

    # Write raw blob: big-endian uint16_t words, suitable for fread into scanout buffer
    bin_path = os.path.join(base, 'splash.bin')
    with open(bin_path, 'wb') as f:
        f.write(struct.pack(f'>{total_words}H', *words))
    print(f"Wrote {bin_path} ({total_words * 2} bytes)")

if __name__ == '__main__':
    main()
