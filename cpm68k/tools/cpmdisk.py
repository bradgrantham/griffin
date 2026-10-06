#!/usr/bin/env python3
"""Make and edit Griffin CP/M-68K drive images (A.CPM .. D.CPM) from the host.

The geometry is the one bios/bios.c implements and tools/diskdefs describes
for cpmtools: 512-byte sectors, 32 per track, 2 KB blocks, 512 directory
entries, no skew, no reserved tracks, 16-bit block numbers, one directory
entry per 16 KB extent (EXM = 0).  The drive's size is the file's size, any
whole number of 2 KB blocks from 32 KB up to 64 MB.

    cpmdisk.py create IMG SIZE_MB            new empty image
    cpmdisk.py ls IMG                        list user 0
    cpmdisk.py put IMG FILE...               copy host files in (user 0, name
                                             uppercased and 8.3-truncated)
    cpmdisk.py get IMG NAME [DEST]           copy a file out
    cpmdisk.py rm IMG NAME...                delete

This exists because the Ubuntu 24.04 cpmtools package (2.23) corrupts its
heap on these images once a few files are on them; tools/diskdefs describes
the same geometry for a cpmtools that works.
"""

import os
import struct
import sys

SECTOR = 512
BLOCK = 2048
DIR_ENTRIES = 512
DIR_BLOCKS = DIR_ENTRIES * 32 // BLOCK          # 8
RECORD = 128
RECORDS_PER_EXTENT = 128                        # 16 KB per directory entry
BLOCKS_PER_EXTENT = RECORDS_PER_EXTENT * RECORD // BLOCK   # 8
EMPTY = 0xE5


def die(msg):
    print(f"cpmdisk: {msg}", file=sys.stderr)
    sys.exit(1)


def cpm_name(host):
    base = os.path.basename(host).upper()
    name, _, ext = base.partition('.')
    name = ''.join(c for c in name if c not in ' <>.,;:=?*[]')[:8]
    ext = ''.join(c for c in ext if c not in ' <>.,;:=?*[]')[:3]
    if not name:
        die(f"'{host}' has no usable CP/M name")
    return name, ext


def pretty(name, ext):
    return name if not ext else f"{name}.{ext}"


class Image:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.data = bytearray(f.read())
        if len(self.data) % BLOCK or len(self.data) < 16 * BLOCK or len(self.data) > 64 << 20:
            die(f"{path}: size must be a multiple of 2 KB between 32 KB and 64 MB")
        self.blocks = len(self.data) // BLOCK

    def save(self):
        with open(self.path, 'wb') as f:
            f.write(self.data)

    def entry(self, i):
        return self.data[i * 32:(i + 1) * 32]

    def set_entry(self, i, e):
        self.data[i * 32:(i + 1) * 32] = e

    def entries(self):
        """(index, user, name, ext, extent, rc, blocks) for every used entry."""
        out = []
        for i in range(DIR_ENTRIES):
            e = self.entry(i)
            if e[0] > 15:
                continue
            name = bytes(c & 0x7F for c in e[1:9]).decode('ascii', 'replace').rstrip()
            ext = bytes(c & 0x7F for c in e[9:12]).decode('ascii', 'replace').rstrip()
            extent = (e[12] & 0x1F) | (e[14] << 5)
            blocks = [b for b in struct.unpack('<8H', e[16:32]) if b]
            out.append((i, e[0], name, ext, extent, e[15], blocks))
        return out

    def used_blocks(self):
        used = set(range(DIR_BLOCKS))
        for ent in self.entries():
            used.update(ent[6])
        return used

    def find(self, name, ext, user=0):
        return sorted((e for e in self.entries() if e[1] == user and e[2] == name and e[3] == ext),
                      key=lambda e: e[4])

    def ls(self):
        files = {}
        for e in self.entries():
            key = (e[1], e[2], e[3])
            files[key] = files.get(key, 0) + len(e[6]) * BLOCK
        for (user, name, ext), size in sorted(files.items()):
            print(f"{user}: {pretty(name, ext):13s} {size:9d}")
        free = self.blocks - len(self.used_blocks())
        print(f"{len(files)} files, {free * BLOCK // 1024} KB free of {self.blocks * BLOCK // 1024} KB")

    def rm(self, name, ext, user=0):
        ents = self.find(name, ext, user)
        if not ents:
            die(f"{pretty(name, ext)}: not found")
        for e in ents:
            ent = self.entry(e[0])
            ent[0] = EMPTY
            self.set_entry(e[0], ent)

    def get(self, name, ext, dest, user=0):
        ents = self.find(name, ext, user)
        if not ents:
            die(f"{pretty(name, ext)}: not found")
        out = bytearray()
        for e in ents:
            chunk = b''.join(self.data[b * BLOCK:(b + 1) * BLOCK] for b in e[6])
            out += chunk[:e[5] * RECORD]
        with open(dest, 'wb') as f:
            f.write(out)

    def put(self, host, user=0):
        name, ext = cpm_name(host)
        if self.find(name, ext, user):
            self.rm(name, ext, user)
        with open(host, 'rb') as f:
            payload = f.read()
        if len(payload) % RECORD:
            payload += b'\x1a' * (RECORD - len(payload) % RECORD)
        records = len(payload) // RECORD
        free = sorted(set(range(DIR_BLOCKS, self.blocks)) - self.used_blocks())
        free_dir = [i for i in range(DIR_ENTRIES) if self.entry(i)[0] > 15]
        n_extents = max(1, (records + RECORDS_PER_EXTENT - 1) // RECORDS_PER_EXTENT)
        need_blocks = (len(payload) + BLOCK - 1) // BLOCK
        if n_extents > len(free_dir):
            die(f"{host}: directory full")
        if need_blocks > len(free):
            die(f"{host}: disk full")
        pos = 0
        for x in range(n_extents):
            ent = bytearray(32)
            ent[0] = user
            ent[1:9] = name.ljust(8).encode('ascii')
            ent[9:12] = ext.ljust(3).encode('ascii')
            ent[12] = x & 0x1F
            ent[14] = x >> 5
            rc = min(RECORDS_PER_EXTENT, records - x * RECORDS_PER_EXTENT)
            ent[15] = rc
            blks = []
            for _ in range(BLOCKS_PER_EXTENT):
                if pos >= len(payload):
                    break
                b = free.pop(0)
                self.data[b * BLOCK:(b + 1) * BLOCK] = payload[pos:pos + BLOCK].ljust(BLOCK, b'\x1a')
                pos += BLOCK
                blks.append(b)
            ent[16:32] = struct.pack('<8H', *(blks + [0] * (8 - len(blks))))
            self.set_entry(free_dir.pop(0), ent)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        sys.exit(2)
    cmd, path = argv[1], argv[2]
    if cmd == 'create':
        if len(argv) != 4:
            die("create IMG SIZE_MB")
        mb = int(argv[3])
        if mb < 1 or mb > 64:
            die("size is 1..64 MB")
        with open(path, 'wb') as f:
            f.write(bytes([EMPTY]) * (mb << 20))
        return
    img = Image(path)
    if cmd == 'ls':
        img.ls()
    elif cmd == 'put':
        for host in argv[3:]:
            img.put(host)
        img.save()
    elif cmd == 'get':
        name, ext = cpm_name(argv[3])
        img.get(name, ext, argv[4] if len(argv) > 4 else pretty(name, ext))
    elif cmd == 'rm':
        for n in argv[3:]:
            img.rm(*cpm_name(n))
        img.save()
    else:
        die(f"unknown command {cmd}")


if __name__ == '__main__':
    main(sys.argv)
