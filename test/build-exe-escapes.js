// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The closed world holds in a produced executable exactly as it does in a
// container. Same cases as test/bundle-escapes.js -- the `require`s that are
// not the loader's own, which a module's wrapper parameter shadows and which
// therefore nothing ordinary meets -- but run from an executable, where the
// bundle root is the executable's own directory rather than the container's.
//
// That relocation is why this file exists rather than being assumed from
// bundle-escapes.js: every identity is re-rooted at the executable, so a
// module tree planted beside the executable sits exactly where the loader's
// own identities live. If anything still consulted the filesystem, this is
// the layout in which it would find something.
//
// REQUIRES: linker-available

// The program. Nothing here names a path from the test: __dirname is the
// executable's directory at run time, which is precisely the directory the
// decoys are planted in.
// RUN: rm -rf %t.src %t.out && mkdir -p %t.src/node_modules/dep %t.out
// RUN: echo '{ "main": "index.js" }' > %t.src/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 5 };" > %t.src/node_modules/dep/index.js
// RUN: echo "module.exports = { v: 9 };" > %t.src/local.js
// RUN: echo "var g = (0, eval)('require');" > %t.src/cli.js
// RUN: echo "try { g(__dirname + '/secret.js'); console.log('EVAL LOADED'); } catch (e) { console.log('EVAL ' + e.code); }" >> %t.src/cli.js
// RUN: echo "try { global.require(__dirname + '/secret.js'); console.log('GLOBAL LOADED'); } catch (e) { console.log('GLOBAL ' + e.code); }" >> %t.src/cli.js
// RUN: echo "try { new Function('return require')()(__dirname + '/secret.js'); console.log('FN LOADED'); } catch (e) { console.log('FN ' + e.code); }" >> %t.src/cli.js
// RUN: echo "var n = './sec' + 'ret'; try { require(n); console.log('COMPUTED LOADED'); } catch (e) { console.log('COMPUTED ' + e.code); }" >> %t.src/cli.js
// RUN: echo "var req = require('module').createRequire(__filename);" >> %t.src/cli.js
// RUN: echo "console.log('CR ' + req('./local').v + ' ' + req('dep').v);" >> %t.src/cli.js
// RUN: echo "try { console.log('CR-RES LEAKED ' + req.resolve('./secret')); } catch (e) { console.log('CR-RES ' + e.code); }" >> %t.src/cli.js
// RUN: echo "console.log('ESC DONE');" >> %t.src/cli.js
// RUN: %hermes-node --build-bundle=%t.src/app.hbb --include=./local --include=dep %t.src/cli.js > /dev/null
// RUN: %hermes-node --build-exe=%t.out/app.exe --kit=%kit_dir %t.src/app.hbb > /dev/null

// A copy of the entry kept for the unbundled control at the bottom of this
// file; the tree it came from is deleted below.
// RUN: cp %t.src/cli.js %t.cli.js

// The decoys, planted beside the executable under the names the bundled
// modules carry -- so `local.js` and `node_modules/dep` are not merely
// present, they are at the identities the container answers for. A loader
// that consulted the disk first would return 77 and 99 instead of 9 and 5,
// and would print DECOY RAN on the way.
//
// The container and its whole source tree are deleted, so nothing below can
// be answered from where it was built either.
// RUN: echo "console.log('SECRET RAN'); module.exports = 1;" > %t.out/secret.js
// RUN: mkdir -p %t.out/node_modules/dep
// RUN: echo '{ "main": "index.js" }' > %t.out/node_modules/dep/package.json
// RUN: echo "console.log('DECOY RAN'); module.exports = { v: 99 };" > %t.out/node_modules/dep/index.js
// RUN: echo "console.log('DECOY RAN'); module.exports = { v: 77 };" > %t.out/local.js
// RUN: rm -rf %t.src

// --implicit-check-not is what does the work for the decoys: a hole here
// prints its extra line and then carries on happily through the CHECKs
// below, so the absence has to be asserted rather than inferred.
// RUN: %t.out/app.exe | %FileCheck --check-prefix=ESCAPE --implicit-check-not="SECRET RAN" --implicit-check-not="DECOY RAN" --implicit-check-not=LOADED %s
// ESCAPE: EVAL MODULE_NOT_FOUND
// ESCAPE-NEXT: GLOBAL MODULE_NOT_FOUND
// ESCAPE-NEXT: FN MODULE_NOT_FOUND
// ESCAPE-NEXT: COMPUTED MODULE_NOT_FOUND
// ESCAPE-NEXT: CR 9 5
// ESCAPE-NEXT: CR-RES MODULE_NOT_FOUND
// ESCAPE-NEXT: ESC DONE

// The same executable, run from a directory that holds no decoys at all,
// must print the same thing. Without this, "the decoys did not win" could be
// satisfied by an executable that fails in some other way once anything
// unexpected is beside it.
// RUN: rm -rf %t.clean && mkdir -p %t.clean
// RUN: cp %t.out/app.exe %t.clean/app.exe
// RUN: %t.clean/app.exe | %FileCheck --check-prefix=ESCAPE --implicit-check-not="SECRET RAN" --implicit-check-not="DECOY RAN" --implicit-check-not=LOADED %s

// The control, and the half that gives every assertion above its meaning:
// the SAME program, unbundled, run by hermes-node from that same directory.
// It finds every decoy -- runs secret.js, prints DECOY RAN twice and reports
// 77 and 99 -- which is what the disk holds and what the executable would
// have printed if any of these routes still reached it. Without this, "the
// escape is blocked" could be satisfied by decoys that were never findable
// in the first place, or by a globalThis.require broken for everyone.
// RUN: cp %t.cli.js %t.out/cli.js
// RUN: %hermes-node %t.out/cli.js | %FileCheck --check-prefix=UNBUNDLED %s
// UNBUNDLED: SECRET RAN
// UNBUNDLED-NEXT: EVAL LOADED
// UNBUNDLED: DECOY RAN
// UNBUNDLED: CR 77 99

// The produced executables are 185 MB apiece under ASAN, so they go when
// they are no longer needed. This is the LAST line deliberately: lit stops
// at the first failing RUN line, so a failure leaves every artifact in place
// for post-mortem and only a passing run cleans up after itself.
// RUN: rm -f %t.out/app.exe %t.clean/app.exe

// This file is a lit driver only; the RUN lines above are the test.
