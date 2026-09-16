#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Count Hermes bytecode blobs in a binary, independently per architecture.

Usage: count-magic.py <binary> <min> [<max>]

Prints one "MAGIC <arch-index> <n>" line per slice and exits non-zero if any
slice falls outside [min, max]. max defaults to min.

EVERY slice, not the average over slices: a universal Mach-O carries a
complete linked image per architecture, and 0 blobs in one slice with 2 in the
other averages to 1, which is exactly the wrong answer passing.

The fat header is parsed here rather than shelling out to `lipo` so the test
needs no extra tool and no shell quoting. A thin file is treated as one slice
covering the whole file.
"""

import struct
import sys

MAGIC = struct.pack("<Q", 0x1F1903C103BC1FC6)

FAT_MAGIC = 0xCAFEBABE      # fat header, 32-bit entries, big-endian fields
FAT_MAGIC_64 = 0xCAFEBABF   # fat header, 64-bit entries
FAT_CIGAM = 0xBEBAFECA      # byte-swapped forms; see slices()
FAT_CIGAM_64 = 0xBFBAFECA


def slices(data):
    """[(offset, size)] per architecture, or one entry for a thin file."""
    if len(data) < 8:
        return [(0, len(data))]
    magic = struct.unpack(">I", data[0:4])[0]
    if magic in (FAT_CIGAM, FAT_CIGAM_64):
        # A little-endian fat header. Apple does not produce these, so rather
        # than write a byte-swapping path nothing here can exercise, refuse:
        # silently treating it as a thin file would count every slice at once
        # and report a number this test would misread as a pass.
        sys.exit("count-magic: byte-swapped fat header is not supported")
    if magic not in (FAT_MAGIC, FAT_MAGIC_64):
        return [(0, len(data))]

    wide = magic == FAT_MAGIC_64
    nfat = struct.unpack(">I", data[4:8])[0]
    entry = 32 if wide else 20
    out = []
    for i in range(nfat):
        at = 8 + i * entry
        if at + entry > len(data):
            sys.exit("count-magic: fat header runs past the end of the file")
        if wide:
            off, size = struct.unpack(">QQ", data[at + 8:at + 24])
        else:
            off, size = struct.unpack(">II", data[at + 8:at + 16])
        if off + size > len(data):
            sys.exit("count-magic: slice %d runs past the end of the file" % i)
        out.append((off, size))
    if not out:
        sys.exit("count-magic: fat header declares no architectures")
    return out


def count(buf):
    n, at = 0, buf.find(MAGIC)
    while at != -1:
        n += 1
        at = buf.find(MAGIC, at + 1)
    return n


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    data = open(sys.argv[1], "rb").read()
    lo = int(sys.argv[2])
    hi = int(sys.argv[3]) if len(sys.argv) == 4 else lo

    bad = False
    for i, (off, size) in enumerate(slices(data)):
        n = count(data[off:off + size])
        print("MAGIC %d %d" % (i, n))
        if n < lo or n > hi:
            print(
                "count-magic: %s slice %d has %d, expected %d..%d"
                % (sys.argv[1], i, n, lo, hi),
                file=sys.stderr,
            )
            bad = True
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
