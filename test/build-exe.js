// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --build-exe end to end: a container goes in, a standalone executable
// comes out, and that executable runs the program with no container, no
// source tree and no hermes-node anywhere near it.
//
// REQUIRES: linker-available
//
// The gate is the `linker-available` feature in test/lit.cfg, which is on
// when lit was given --param kit_dir naming a directory that holds a
// kit.manifest. check-hermes-node-js depends on the hermes-node-kit target,
// so an ordinary suite run has one; a bare hermes-lit invocation without the
// param reports this UNSUPPORTED instead of failing it.

// One entry, one required module, one JSON file and one --preload, so that
// the produced executable exercises a module graph rather than a single
// file, and so the container's preload table is carried into it.
// RUN: rm -rf %t.app && mkdir -p %t.app/lib
// RUN: echo "globalThis.__preloadRan = true; console.log('PRELOAD RAN');" > %t.app/pre.js
// RUN: echo "module.exports = { v: 10 };" > %t.app/lib/dep.js
// RUN: echo '{ "v": 100 }' > %t.app/cfg.json
// RUN: echo "const d = require('./lib/dep'); const c = require('./cfg.json');" > %t.app/main.js
// RUN: echo "console.log('MARKER_BUILD_EXE_PAYLOAD');" >> %t.app/main.js
// RUN: echo "console.log('PASS ' + (40 + 2));" >> %t.app/main.js
// RUN: echo "console.log('SUM ' + (d.v + c.v));" >> %t.app/main.js
// RUN: echo "console.log('PRELOADED ' + (globalThis.__preloadRan === true));" >> %t.app/main.js
// RUN: echo "console.log('ARGC ' + process.argv.length + ' ARGS ' + process.argv.slice(2).join(','));" >> %t.app/main.js
// RUN: echo "console.log('ARGV01 ' + (process.argv[0] === process.argv[1]));" >> %t.app/main.js
// RUN: echo "console.log('ROOT ' + (__dirname === require('path').dirname(process.execPath)));" >> %t.app/main.js
// RUN: echo "if (process.argv[2] === 'exit3') { process.exit(3); }" >> %t.app/main.js
// RUN: echo "if (process.argv[2] === 'throw') { throw new Error('BOOM'); }" >> %t.app/main.js
// RUN: %hermes-node --build-bundle=%t.app/app.hbb --preload=./pre.js %t.app/main.js

// A copy kept well away from the executable. The container beside the
// executable is deleted below, and the payload-corruption case at the end
// still needs the container's bytes to do its offset arithmetic against.
// RUN: cp %t.app/app.hbb %t.keep.hbb

// The link itself. It reports what it wrote, in the shape the other
// producers report it.
// RUN: %hermes-node --build-exe=%t.app/app.exe --kit=%kit_dir %t.app/app.hbb | %FileCheck --check-prefix=WROTE %s
// WROTE: wrote {{.*}}app.exe ({{[0-9]+}} bytes)

// The temporaries the link goes through (a multi-megabyte .s and the .o
// assembled from it) are written beside the output and must not survive it.
// RUN: %not ls %t.app/app.exe.hnexe.*

// Running it. `one two` are the user's arguments: process.argv is
// [exe, exe, one, two], so slice(2) is what an argument parser sees, exactly
// as under --bundle. ROOT is the property the natives test depends on -- a
// produced executable's bundle root is its own directory.
// RUN: %t.app/app.exe one two | %FileCheck --check-prefix=APP %s
// APP: PRELOAD RAN
// APP-NEXT: MARKER_BUILD_EXE_PAYLOAD
// APP-NEXT: PASS 42
// APP-NEXT: SUM 110
// APP-NEXT: PRELOADED true
// APP-NEXT: ARGC 4 ARGS one,two
// APP-NEXT: ARGV01 true
// APP-NEXT: ROOT true

// The whole point of the feature, and the one case that cannot be faked:
// delete the container and the entire source tree, and run the same
// executable again. Anything still reading either would fail here.
// RUN: rm -f %t.app/app.hbb %t.app/main.js %t.app/pre.js %t.app/cfg.json
// RUN: rm -rf %t.app/lib
// RUN: %not ls %t.app/app.hbb
// RUN: %t.app/app.exe one two | %FileCheck --check-prefix=APP %s

// The program's exit status is the executable's. Both routes to a non-zero
// one are pinned, and by their exact value rather than with %not, which
// would pass just as happily on an executable that died on startup:
// process.exit(3), which leaves through exit(), and an uncaught throw, whose
// 1 is runHermesNode()'s return value carried out through the produced
// binary's own main().
//
// process.exitCode -- the assignable property -- is NOT tested here because
// this runtime does not honour it in any mode (a plain script, --bundle and
// a produced executable all exit 0 after setting it). That gap is not
// specific to --build-exe and pinning it here would misattribute it.
// RUN: %t.app/app.exe exit3 > /dev/null; echo "EXITCODE $?" | %FileCheck --check-prefix=EXIT3 %s
// EXIT3: EXITCODE 3
// RUN: %t.app/app.exe throw > /dev/null 2> %t.app/throw.err; echo "EXITCODE $?" | %FileCheck --check-prefix=EXIT1 %s
// EXIT1: EXITCODE 1
// RUN: %FileCheck --check-prefix=THROW %s < %t.app/throw.err
// THROW: BOOM

// A bad payload. Nothing the producer accepts can build one -- it validates
// the container before it links -- so this checks the embedded run path
// itself, reached by corrupting the payload inside a finished executable.
// The magic is the container's first eight bytes and the payload is the
// container verbatim, so a string unique to the program sits the same
// distance past the start of both files. The `test` line asserts the marker
// really is unique in the executable, so a future build that inlined it a
// second time fails loudly here rather than quietly corrupting some other
// part of the binary and testing nothing.
// RUN: test $(grep -abo MARKER_BUILD_EXE_PAYLOAD %t.app/app.exe | wc -l) = 1
// RUN: OE=$(grep -abo MARKER_BUILD_EXE_PAYLOAD %t.app/app.exe | cut -d: -f1); OH=$(grep -abo MARKER_BUILD_EXE_PAYLOAD %t.keep.hbb | cut -d: -f1); printf 'X' | dd of=%t.app/app.exe bs=1 seek=$(($OE - $OH)) count=1 conv=notrunc 2>/dev/null
// RUN: %not %t.app/app.exe 2>&1 | %FileCheck --check-prefix=BADPAYLOAD %s
// BADPAYLOAD: error: hermes-node bundle: not a hermes-node bundle (bad magic)

// The produced executables are 185 MB apiece under ASAN, so they go when
// they are no longer needed. This is the LAST line deliberately: lit stops
// at the first failing RUN line, so a failure leaves every artifact in place
// for post-mortem and only a passing run cleans up after itself.
// RUN: rm -f %t.app/app.exe %t.keep.hbb

// This file is a lit driver only; the RUN lines above are the test.
