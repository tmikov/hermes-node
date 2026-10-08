#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Write an empty format-v1 --record-wasm file to argv[1].

What every hermes-node before record format v2 wrote, and what the current
one no longer can: v1 carries bytecode only, with no module bytes for a
native build to compile. Laid out exactly as the v1 writer did
(lib/bundle/wasm_record.cpp at format v1): a 36-byte header, the build
version string, the record table at a 4-byte boundary, the payload at an
8-byte one.
"""

import struct
import sys

VERSION = b"v1-fixture"
HEADER = 36
table = (HEADER + len(VERSION) + 3) & ~3
payload = (table + 7) & ~7
data = b"HNWASMRC" + struct.pack(
    "<7I", 1, 0, HEADER, len(VERSION), table, payload, 0)
data += VERSION
data += b"\0" * (payload - len(data))
with open(sys.argv[1], "wb") as f:
    f.write(data)
