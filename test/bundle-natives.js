// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A bundled program that requires a native addon: the addon is packaged as
// a kNative record and its bytes ship as a flat sidecar next to the
// container.
//
// Every check under the BUILD prefix here is stdout, and every check under
// a *V prefix is stderr, deliberately: with `2>&1` the two streams
// interleave by buffering rather than by order (stdout is block-buffered
// into a pipe, stderr is not), so a prefix that mixed them would pass or
// fail on a detail no one intends to pin.

// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: cp %hello_addon %t.dir/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.dir/main.js
// RUN: %hermes-node --build-bundle=%t.dir/app.hbb %t.dir/main.js 2>&1 | %FileCheck --check-prefix=BUILD %s
// BUILD: bundle root: {{.*}}
// BUILD: native: hello_addon.node (from hello_addon.node)
// BUILD: note: this bundle requires 1 native addon alongside it; ship them together.

// The payoff: require('./hello_addon.node') inside the running bundle
// actually dlopens the sidecar and the addon's exports work.
// RUN: %hermes-node --bundle=%t.dir/app.hbb | %FileCheck %s
// CHECK: PASS

// The sidecar, not the original, is what runs: build into a directory that
// holds no addon of its own and confirm the program still works. If the
// loader were somehow still reaching for the source tree's copy -- or for
// anything under %t.dir -- this would fail, since %t.out has no addon
// until the build copies one there.
// RUN: rm -rf %t.out && mkdir -p %t.out
// RUN: %hermes-node --build-bundle=%t.out/app.hbb %t.dir/main.js > /dev/null
// RUN: ls %t.out/hello_addon.node
// RUN: %hermes-node --bundle=%t.out/app.hbb | %FileCheck %s

// The addon here already sits at the bundle root under the name its sidecar
// would take, so there is nothing to copy and the build must not "copy" it
// onto itself -- that would replace one of its own inputs with a fresh
// inode and mtime for no gain. It is still a native the bundle requires, so
// it is still recorded and still announced above. That the file survived
// the build at all is what `ls` asserts.
// RUN: ls %t.dir/hello_addon.node

// A SYMLINK at the sidecar path is not "already in place". stat() follows
// links, so the same-file test that makes the case above skip its copy also
// answers yes here -- and skipping would leave the sidecar a link into the
// source tree: the output directory alone would then be a bundle that
// throws once that tree is gone, and --verify-natives (which reads through
// the link) would call it OK. The copy must happen and must replace the
// link with a regular file holding the addon's bytes.
// RUN: rm -rf %t.link.dir %t.link.src && mkdir -p %t.link.dir %t.link.src
// RUN: cp %hello_addon %t.link.src/hello_addon.node
// RUN: echo "require('%t.link.src/hello_addon.node');" > %t.link.dir/main.js
// RUN: ln -s %t.link.src/hello_addon.node %t.link.dir/hello_addon.node
// RUN: %hermes-node --build-bundle=%t.link.dir/app.hbb %t.link.dir/main.js > /dev/null
// RUN: %not readlink %t.link.dir/hello_addon.node
// RUN: cmp %t.link.dir/hello_addon.node %t.link.src/hello_addon.node
// RUN: rm -rf %t.link.src
// RUN: %hermes-node --bundle=%t.link.dir/app.hbb --verify-natives | %FileCheck --check-prefix=LINKOK %s
// LINKOK: OK {{ *}}hello_addon.node

// An addon somewhere under the root, where the sidecar is a real copy. The
// narration names both ends of it, and the summary counts the natives on a
// line of their own: those bytes are beside the container, not in it, so
// adding them into `total:` would describe a file that does not exist.
// RUN: rm -rf %t.sub && mkdir -p %t.sub/native
// RUN: cp %hello_addon %t.sub/native/hello_addon.node
// RUN: echo "const a = require('./native/hello_addon.node'); console.log('V', a.hello());" > %t.sub/main.js
// RUN: %hermes-node --build-bundle=%t.sub/app.hbb --verbose %t.sub/main.js 2>&1 | %FileCheck --check-prefix=SUBV %s
// SUBV: native  native/hello_addon.node -> hello_addon.node
// SUBV-NEXT: from {{.*}}/native/hello_addon.node
// SUBV-NEXT: to {{.*}}/hello_addon.node
// SUBV-NEXT: {{[0-9]+}} bytes sha256:{{[0-9a-f]+}}
// SUBV: modules: 2 (1 js, 0 json, 1 native)
// SUBV: natives: 1 file, {{[0-9]+}} bytes alongside (not in the container)
// RUN: ls %t.sub/hello_addon.node

// Two addons with the same basename cannot both be `hello_addon.node` in
// one flat directory. The second gets a short hash of its identity, which
// is stable across builds and independent of discovery order -- and the
// container records the map, so nothing at run time has to re-derive it.
// RUN: rm -rf %t.coll && mkdir -p %t.coll/a %t.coll/b
// RUN: cp %hello_addon %t.coll/a/hello_addon.node
// RUN: cp %hello_addon %t.coll/b/hello_addon.node
// RUN: echo "require('./a/hello_addon.node'); require('./b/hello_addon.node');" > %t.coll/main.js
// RUN: %hermes-node --build-bundle=%t.coll/app.hbb %t.coll/main.js | %FileCheck --check-prefix=COLL %s
// COLL: native: hello_addon.node (from a/hello_addon.node)
// COLL: native: hello_addon-{{[0-9a-f]+}}.node (from b/hello_addon.node)
// COLL: note: this bundle requires 2 native addons alongside it; ship them together.
// RUN: ls %t.coll | %FileCheck --check-prefix=COLLLS %s
// COLLLS: hello_addon-{{[0-9a-f]+}}.node

// The shape that used to overwrite one of the build's own inputs: an addon
// beside the entry, and a second addon with the SAME basename under
// node_modules -- proj/binding.node and
// node_modules/foo/build/Release/binding.node, which is about as ordinary a
// pair as this ecosystem produces. The entry requires foo's copy first, so
// discovery order alone would hand it the plain name `binding.node`, whose
// destination IS the other addon's source file. An addon that already is
// the file it would be copied to therefore gets first claim on its plain
// basename, and the other takes the hashed one.
//
// The two addons are made distinguishable by appending a marker, so the
// assertions below are about bytes and not just about names: the root
// addon's file must be untouched by the build, and each sidecar must hold
// its own addon.
//
// 1ad0db51 is crc32("node_modules/foo/build/Release/binding.node"), which
// is a function of the identity alone -- not of the machine, the file or
// the discovery order. If the disambiguation rule ever changes, this line
// fails loudly rather than silently stopping to test anything.
// RUN: rm -rf %t.root && mkdir -p %t.root/node_modules/foo/build/Release
// RUN: cp %hello_addon %t.root/binding.node && printf 'ROOTADDON' >> %t.root/binding.node
// RUN: cp %hello_addon %t.root/node_modules/foo/build/Release/binding.node
// RUN: printf 'FOOADDON' >> %t.root/node_modules/foo/build/Release/binding.node
// RUN: cp %t.root/binding.node %t.root-before
// RUN: echo "require('./node_modules/foo/build/Release/binding.node'); require('./binding.node');" > %t.root/main.js
// RUN: %hermes-node --build-bundle=%t.root/app.hbb %t.root/main.js | %FileCheck --check-prefix=ROOTFIRST %s
// ROOTFIRST: native: binding-1ad0db51.node (from node_modules/foo/build/Release/binding.node)
// ROOTFIRST: native: binding.node (from binding.node)
// RUN: cmp %t.root/binding.node %t.root-before
// RUN: cmp %t.root/binding-1ad0db51.node %t.root/node_modules/foo/build/Release/binding.node

// A disambiguated name that is ALSO taken is a hard build error: shipping
// one addon's bytes under another addon's name would be silent (the
// container records only the sidecar name) and wrong. Reached by planting a
// third addon whose real basename is the hashed name the second one will
// want -- 2beec696 is crc32("b/hello_addon.node"), stable for the same
// reason 1ad0db51 above is -- and requiring it first, so it holds the name
// before the collision happens.
// RUN: rm -rf %t.double && mkdir -p %t.double/a %t.double/b %t.double/c
// RUN: cp %hello_addon %t.double/a/hello_addon.node
// RUN: cp %hello_addon %t.double/b/hello_addon.node
// RUN: cp %hello_addon %t.double/c/hello_addon-2beec696.node
// RUN: echo "require('./c/hello_addon-2beec696.node'); require('./a/hello_addon.node'); require('./b/hello_addon.node');" > %t.double/main.js
// RUN: %not %hermes-node --build-bundle=%t.double/app.hbb %t.double/main.js 2>&1 | %FileCheck --check-prefix=DOUBLE %s
// DOUBLE: error: native addons c/hello_addon-2beec696.node and b/hello_addon.node both want the sidecar file hello_addon-2beec696.node
// RUN: %not ls %t.double/app.hbb

// An addon whose sidecar name is the bundle's own is refused before
// anything is written. Nothing exists in this directory yet, so the
// (st_dev, st_ino) test that catches a rebuild has nothing to compare and
// the spelling test is what catches it -- which is the half a regression
// would remove, leaving the build to copy the addon onto its output path
// and then overwrite it with the container.
// RUN: rm -rf %t.own && mkdir -p %t.own/native
// RUN: cp %hello_addon %t.own/native/app.node
// RUN: echo "require('./native/app.node');" > %t.own/main.js
// RUN: %not %hermes-node --build-bundle=%t.own/app.node %t.own/main.js 2>&1 | %FileCheck --check-prefix=OWN %s
// OWN: error: native addon {{.*}}native/app.node would be written over the bundle {{.*}}app.node
// RUN: %not ls %t.own/app.node

// __bundleLoad must refuse a native: its bytes are not in the container,
// and a loader that tried would be running a shared object as bytecode.
// RUN: cp %hello_addon %t.dir/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/load-refused.js %t.dir/refused.js
// RUN: %hermes-node --build-bundle=%t.dir/refused.hbb --include=./hello_addon.node %t.dir/refused.js > /dev/null
// RUN: %hermes-node --bundle=%t.dir/refused.hbb | %FileCheck --check-prefix=REFUSED %s
// REFUSED: PASS refused

// This file is a lit driver only; the RUN lines above are the test.
