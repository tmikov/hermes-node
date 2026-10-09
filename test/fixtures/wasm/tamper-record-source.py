#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Flip one byte of the first module-bytes entry of a format-v2
--record-wasm file, in place, leaving its recorded digest alone.

What a record damaged after it was written looks like to the bake step:
structurally valid (the reader checks offsets, not contents), but the
bytes no longer hash to the digest they are filed under. Laid out as
include/hermes/node-compat/bundle/wasm_record.h describes it: a 44-byte
header of eight-byte magic and nine uint32 fields, and a source table of
32-byte digest plus payloadOffset and payloadSize, the offset relative to
the header's payloadOffset.
"""

import struct
import sys

path = sys.argv[1]
with open(path, "rb") as f:
    data = bytearray(f.read())
if data[:8] != b"HNWASMRC":
    sys.exit("%s: not a --record-wasm file" % path)
(version, _count, _voff, _vlen, _rtab, payload, _psize, stab,
 scount) = struct.unpack_from("<9I", data, 8)
if version != 2 or scount == 0:
    sys.exit("%s: need a format v2 file with a source entry" % path)
off, size = struct.unpack_from("<2I", data, stab + 32)
if size == 0:
    sys.exit("%s: first source entry is empty" % path)
data[payload + off + size - 1] ^= 0xFF
with open(path, "wb") as f:
    f.write(data)
