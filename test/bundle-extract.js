// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: rm -rf %t.tree && mkdir -p %t.tree/lib
// RUN: echo "const u = require('./lib/util'); const c = require('./cfg.json'); console.log('D', u.v + c.v);" > %t.tree/cli.js
// RUN: echo "module.exports = { v: 1 };" > %t.tree/lib/util.js
// RUN: echo '{ "v": 2 }' > %t.tree/cfg.json
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb %t.tree/cli.js

// Extracting a JavaScript module writes its payload out verbatim: no
// header, no transformation. What lands in %t.util.hbc is exactly the
// bytecode hermes_compile_to_bytecode produced for lib/util.js.
// RUN: %hermes-node --bundle=%t.tree/app.hbb --extract-module=lib/util.js --out=%t.util.hbc
//
// What makes that claim checkable is that the extracted file disassembles:
// Hermes accepts it as a bytecode file with no preprocessing of any kind.
// RUN: %hermes-node --dump-bytecode=%t.util.hbc | %FileCheck --check-prefix=BC %s
// BC: Bytecode File Information

// Extracting a JSON module round-trips byte-identically: its payload is
// the source file's own bytes, unmodified.
// RUN: %hermes-node --bundle=%t.tree/app.hbb --extract-module=cfg.json --out=%t.cfg.json
// RUN: cmp %t.cfg.json %t.tree/cfg.json

// An unknown identity is a hard error, and a close typo is offered a
// suggestion drawn from the container's own identities.
// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --extract-module=lib/utl.js --out=%t.x 2>&1 | %FileCheck --check-prefix=TYPO %s
// TYPO: no module 'lib/utl.js'
// TYPO: did you mean
// TYPO: lib/util.js

// Extraction must not have written anything on failure: the write never
// gets far enough to attempt the temp-file-then-rename when the module
// name does not resolve to an index in the first place.
// RUN: test ! -e %t.x

// A wild typo -- one not close to any real identity -- gets no suggestion
// list at all, rather than three irrelevant ones padded in to fill it.
// RUN: %not %hermes-node --bundle=%t.tree/app.hbb --extract-module=completely-unrelated-name --out=%t.y 2>&1 | %FileCheck --check-prefix=NOSUGGEST --implicit-check-not="did you mean" %s
// NOSUGGEST: no module 'completely-unrelated-name'
// RUN: test ! -e %t.y

// Extracting onto the container itself is refused, and the container is
// left exactly as it was. Without the guard the rename replaces a 1000+
// byte container with a 400-byte module payload, exits 0, and says nothing
// -- the mapping still holds the old inode, so nothing downstream notices.
// A container built by another hermes-node is the headline case for these
// verbs, and it is precisely the one that cannot be rebuilt.
// RUN: cp %t.tree/app.hbb %t.tree/guard.hbb
// RUN: %not %hermes-node --bundle=%t.tree/guard.hbb --extract-module=lib/util.js --out=%t.tree/guard.hbb 2>&1 | %FileCheck --check-prefix=SELF %s
// SELF: error: {{.*}}names the same file as the bundle
// RUN: cmp %t.tree/guard.hbb %t.tree/app.hbb

// Same file, different spelling: the guard compares (st_dev, st_ino), not
// two strings, so a symlink to the container is refused too.
// RUN: ln -sf %t.tree/guard.hbb %t.tree/link.hbb
// RUN: %not %hermes-node --bundle=%t.tree/guard.hbb --extract-module=lib/util.js --out=%t.tree/link.hbb 2>&1 | %FileCheck --check-prefix=SELFLINK %s
// SELFLINK: error: {{.*}}names the same file as the bundle
// RUN: cmp %t.tree/guard.hbb %t.tree/app.hbb

// A different file next to the container is written as normal: the guard
// refuses one file, not a directory.
// RUN: %hermes-node --bundle=%t.tree/guard.hbb --extract-module=lib/util.js --out=%t.tree/guard.hbb.util
// RUN: cmp %t.tree/guard.hbb.util %t.util.hbc

// Inspection mode, exactly like --dump: extraction works on a container
// from a mismatched generation, which the binary otherwise refuses to run
// at all. Getting bytecode out of a file this binary cannot execute is the
// reason the feature exists, not a case it declines to handle. Recipe
// (stamp four 0xff bytes over the generation tag at offset 12) borrowed
// from bundle-dump.js's identical MISMATCH test.
// RUN: cp %t.tree/app.hbb %t.tree/old.hbb
// RUN: printf '\xff\xff\xff\xff' | dd of=%t.tree/old.hbb bs=1 seek=12 count=4 conv=notrunc 2>/dev/null
// RUN: %not %hermes-node --bundle=%t.tree/old.hbb 2>&1 | %FileCheck --check-prefix=OLDGEN %s
// OLDGEN: generation mismatch
// RUN: %hermes-node --bundle=%t.tree/old.hbb --extract-module=cfg.json --out=%t.oldgen.json
// RUN: cmp %t.oldgen.json %t.tree/cfg.json

// This file is a lit driver only; the RUN lines above are the test.
