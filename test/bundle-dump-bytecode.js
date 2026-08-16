// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --dump-bytecode disassembles a file of Hermes bytecode. The two shapes it
// accepts are the two this binary produces: a raw bytecode file, such as
// --extract-module writes out of a bundle, and a compile cache entry, which
// is the same bytecode behind a 24-byte header.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib
// RUN: echo "const u = require('./lib/util'); console.log('D', u.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb --extract-module=lib/util.js --out=%t.util.hbc

// The raw case: a bytecode file's header, its functions, and the byte range
// of every section. util.js compiles to two functions -- the module's own
// global function and the closure holding its body -- and pinning that count
// is what keeps this from passing on an empty disassembly.
// RUN: %hermes-node --dump-bytecode=%t.util.hbc | %FileCheck %s
// CHECK: bytecode: {{.*}}util.hbc
// CHECK: Bytecode File Information:
// CHECK: Function count: 2
// CHECK: Function<global>0(
// CHECK: Ret
// CHECK: Byte range of each section in bytecode:
// CHECK: Function body: [{{[0-9]+}}, {{[0-9]+}})

// Source lines are a --verbose addition, not the default. The exclusion is
// --implicit-check-not rather than a NOT line because a NOT line only covers
// the region up to the next positive match, and the source comments are
// interleaved with the instructions rather than confined to one region.
// RUN: %hermes-node --dump-bytecode=%t.util.hbc | %FileCheck --check-prefix=QUIET --implicit-check-not="util.js:1:2" %s
// QUIET: Function<global>0(

// RUN: %hermes-node --dump-bytecode=%t.util.hbc --verbose | %FileCheck --check-prefix=SRC %s
// SRC: ; {{.*}}lib/util.js:1:2

// The compile cache case: an entry is the same bytecode behind a 24-byte
// header, and the cache directory is full of them, so a user pointing this
// at one gets a disassembly rather than advice about hexdumping past a
// header. The cache is off suite-wide (see test/lit.cfg), which is why the
// producing run goes through %hermes-node-cc.
// RUN: rm -rf %t.cc && echo "console.log('CC');" > %t.cc.js
// RUN: %hermes-node-cc --compile-cache=%t.cc %t.cc.js
// RUN: %hermes-node --dump-bytecode=$(find %t.cc -type f | head -n 1) | %FileCheck --check-prefix=CACHE %s
// CACHE: bytecode: {{.*}}compile cache entry
// CACHE: Bytecode File Information:
// CACHE: Function count: {{[0-9]+}}
// CACHE: Byte range of each section in bytecode:

// A file that is not bytecode is Hermes's own diagnosis, reported verbatim,
// with a non-zero exit and no crash.
// RUN: echo "not bytecode at all" > %t.notbc
// RUN: %not %hermes-node --dump-bytecode=%t.notbc 2>&1 | %FileCheck --check-prefix=SHORT %s
// SHORT: error: {{.*}}notbc: Buffer smaller than a bytecode file header

// Empty, and large-but-not-bytecode: the two ends of the same case. 200000
// zero bytes is past the header size, so it reaches the magic check rather
// than the length check, and it is deterministic, which /dev/urandom would
// not be.
// RUN: printf '' > %t.empty
// RUN: %not %hermes-node --dump-bytecode=%t.empty 2>&1 | %FileCheck --check-prefix=EMPTY %s
// EMPTY: error: {{.*}}empty: Buffer smaller than a bytecode file header

// RUN: head -c 200000 /dev/zero > %t.big
// RUN: %not %hermes-node --dump-bytecode=%t.big 2>&1 | %FileCheck --check-prefix=BIG %s
// BIG: error: {{.*}}big: Incorrect magic number

// A file carrying the compile cache magic (bytes 4e 48 43 43, header
// version 1) but a bytecodeSize of 0xffffffff, which its 224 bytes plainly
// cannot hold. The cache directory is user-writable, so a header claiming
// more than the file holds is reachable; the size has to be checked against
// the file rather than trusted, and a run that trusted it would copy 4 GB
// out of a 224-byte allocation. The 200 bytes of padding are not what makes
// that overread reachable -- 0xffffffff runs off a 24-byte allocation just
// as surely -- they are here so the file is large enough that nothing
// downstream can reject it for its length instead of its header.
// RUN: printf '\x4e\x48\x43\x43\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\x00\x00\x00\x00' > %t.badcc
// RUN: head -c 200 /dev/zero >> %t.badcc
// RUN: %not %hermes-node --dump-bytecode=%t.badcc 2>&1 | %FileCheck --check-prefix=BADCC %s
// BADCC: error: {{.*}}badcc: compile cache entry rejected: header claims 4294967295 bytes of bytecode, but only 200 bytes follow its header

// The other ways a cache header can be rejected say so too, rather than
// blaming a bytecode magic that was never the problem: a real entry with
// its header version bumped to 2 is the case a user actually meets, when a
// future binary changes the layout and they point this at an old entry.
// The dd seek lands on the header's second field.
// RUN: cp $(find %t.cc -type f | head -n 1) %t.oldver
// RUN: printf '\x02' | dd of=%t.oldver bs=1 seek=4 count=1 conv=notrunc 2>/dev/null
// RUN: %not %hermes-node --dump-bytecode=%t.oldver 2>&1 | %FileCheck --check-prefix=OLDVER %s
// OLDVER: error: {{.*}}oldver: compile cache entry rejected: header version 2, this binary reads version 1

// A real entry truncated by one byte: the header is intact and its size
// field now overruns the file by one.
// RUN: SZ=$(wc -c < $(find %t.cc -type f | head -n 1)); head -c $(($SZ - 1)) $(find %t.cc -type f | head -n 1) > %t.trunc
// RUN: %not %hermes-node --dump-bytecode=%t.trunc 2>&1 | %FileCheck --check-prefix=TRUNC %s
// TRUNC: error: {{.*}}trunc: compile cache entry rejected: header claims {{[0-9]+}} bytes of bytecode, but only {{[0-9]+}} bytes follow its header

// A header that is intact and records no bytecode at all. Nothing follows
// it to disassemble, so handing the 200 bytes of padding to Hermes would
// report a bad bytecode magic -- true of the padding, and not the problem.
// The header below is the cache magic, header version 1, then zeroes, which
// puts a bytecodeSize of 0 at offset 16.
// RUN: printf '\x4e\x48\x43\x43\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' > %t.zerocc
// RUN: head -c 200 /dev/zero >> %t.zerocc
// RUN: %not %hermes-node --dump-bytecode=%t.zerocc 2>&1 | %FileCheck --check-prefix=ZEROCC %s
// ZEROCC: error: {{.*}}zerocc: compile cache entry rejected: header records no bytecode

// Shorter than the 24-byte header itself: eight bytes carrying the cache
// magic and a header version, and nothing else. Every field the checks
// below it read (the bytecodeSize at offset 16) is off the end of the file,
// so this case has to be caught before any of them is read.
// RUN: printf '\x4e\x48\x43\x43\x01\x00\x00\x00' > %t.shortcc
// RUN: %not %hermes-node --dump-bytecode=%t.shortcc 2>&1 | %FileCheck --check-prefix=SHORTCC %s
// SHORTCC: error: {{.*}}shortcc: compile cache entry rejected: truncated: 8 bytes, shorter than the 24-byte entry header

// A bundle container is the wrong tool, and gets named rather than
// reported as a bad bytecode magic. The note points at the two verbs that
// do read a container -- one of which produces a file this can read.
// RUN: %not %hermes-node --dump-bytecode=%t.tree/app.hbb 2>&1 | %FileCheck --check-prefix=CONTAINER %s
// CONTAINER: error: {{.*}}app.hbb: this is a bundle container, not a file of bytecode
// CONTAINER: note: --bundle={{.*}}app.hbb --dump
// CONTAINER-SAME: --extract-module=

// A path that does not exist reports why, rather than an empty disassembly.
// RUN: rm -f %t.missing
// RUN: %not %hermes-node --dump-bytecode=%t.missing 2>&1 | %FileCheck --check-prefix=MISSING %s
// MISSING: error: {{.*}}missing: No such file or directory

// This file is a lit driver only; the RUN lines above are the test.
