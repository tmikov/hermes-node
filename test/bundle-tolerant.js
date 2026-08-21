// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The producer's static walk over-approximates what a program loads: it
// follows every literal require() whether or not the run ever reaches it.
// Two things it finds there are ordinary in real packages and fatal to
// neither Node nor this runtime -- a specifier that resolves to nothing
// (how a package probes for an optional dependency) and a file this engine
// cannot compile (an .mjs-flavored branch, a dynamic import()) that the
// program never requires. Both used to fail the build.
//
// Now both are warnings, and the container reproduces what running from
// disk does: the unresolved specifier goes to the run-time loader, which
// throws MODULE_NOT_FOUND, and the uncompilable file becomes a module that
// throws its own SyntaxError if -- and only if -- something requires it.
// The entry point is the exception, and stays a hard error: see
// test/bundle-errors.js.

// An unresolvable specifier warns, and the build succeeds.
// RUN: rm -rf %t.miss && mkdir -p %t.miss
// RUN: echo "try { require('nope-not-here'); } catch (e) { console.log('MISSING', e.code); } console.log('MISS DONE');" > %t.miss/cli.js
// RUN: %hermes-node --build-bundle=%t.miss/app.hbb %t.miss/cli.js 2>&1 | %FileCheck --check-prefix=MISSWARN %s
// MISSWARN: warning: not packaging 'nope-not-here' from {{.*}}cli.js (cannot be resolved, left to the run-time loader)

// The require() throws the same catchable MODULE_NOT_FOUND it throws from
// disk -- pinning e.code, not merely that something was thrown, because a
// package's optional-dependency probe branches on exactly that value.
// RUN: %hermes-node --bundle=%t.miss/app.hbb | %FileCheck --check-prefix=MISSEXEC %s
// MISSEXEC: MISSING MODULE_NOT_FOUND
// MISSEXEC: MISS DONE

// A file the compiler rejects warns, and the build succeeds.
// RUN: rm -rf %t.stub && mkdir -p %t.stub
// RUN: echo "module.exports = function (f) { return import(f); };" > %t.stub/dyn.cjs
// RUN: echo "console.log('BEFORE'); try { require('./dyn.cjs'); } catch (e) { console.log('THREW', e.name); } console.log('AFTER');" > %t.stub/cli.js
// RUN: %hermes-node --build-bundle=%t.stub/app.hbb %t.stub/cli.js 2>&1 | %FileCheck --check-prefix=STUBWARN %s
// STUBWARN: warning: cannot compile {{.*}}dyn.cjs (SyntaxError: {{.*}}); packaged as a module that throws when required

// Requiring it throws a SyntaxError, and nothing else about the run
// changes: the lines on either side of the require() are what say the
// throw happened at the require and not before it.
// RUN: %hermes-node --bundle=%t.stub/app.hbb | %FileCheck --check-prefix=STUBEXEC %s
// STUBEXEC: BEFORE
// STUBEXEC: THREW SyntaxError
// STUBEXEC: AFTER

// The stub is IN the container. With the whole source tree deleted the
// exception is unchanged -- which is what distinguishes packaging a stub
// from leaving the file out: leaving it out would surface
// Error/MODULE_NOT_FOUND here instead, from the closed world's own
// not-found path. Nothing consults the filesystem either way, so the tree
// being gone is not what produces the SyntaxError; the container is.
// RUN: rm -f %t.stub/cli.js %t.stub/dyn.cjs
// RUN: %hermes-node --bundle=%t.stub/app.hbb | %FileCheck --check-prefix=STUBGONE %s
// STUBGONE: THREW SyntaxError

// The motivating case: the program never requires the file at all, so the
// stub is never evaluated and the run is completely unaffected. This is
// what a real package ships -- a branch for another module system, behind a
// condition this runtime never takes.
// RUN: rm -rf %t.quiet && mkdir -p %t.quiet
// RUN: echo "module.exports = function (f) { return import(f); };" > %t.quiet/dyn.cjs
// RUN: echo "if (globalThis.never) { require('./dyn.cjs'); } console.log('QUIET OK');" > %t.quiet/cli.js
// RUN: %hermes-node --build-bundle=%t.quiet/app.hbb %t.quiet/cli.js 2>&1 | %FileCheck --check-prefix=QUIETWARN %s
// QUIETWARN: warning: cannot compile {{.*}}dyn.cjs
// RUN: rm -f %t.quiet/cli.js %t.quiet/dyn.cjs
// RUN: %hermes-node --bundle=%t.quiet/app.hbb | %FileCheck --check-prefix=QUIETEXEC %s
// QUIETEXEC: QUIET OK

// A top-level `return` is an ordinary CommonJS early-exit idiom, legal
// because a module body is a function body. The scan wraps the source in
// the module wrapper before parsing it for exactly this reason; reading it
// as a Program rejects it, which used to fail the build outright and then,
// once failures became stubs, silently turned a working module into one
// that throws. Neither is acceptable for a file that runs from disk.
// RUN: rm -rf %t.ret && mkdir -p %t.ret
// RUN: echo "if (globalThis.never) { module.exports = { v: 1 }; return; }" > %t.ret/dep.js
// RUN: echo "module.exports = { v: 2 };" >> %t.ret/dep.js
// RUN: echo "console.log('RETURN', require('./dep').v);" > %t.ret/cli.js
// RUN: %hermes-node --build-bundle=%t.ret/app.hbb %t.ret/cli.js 2>&1 | %FileCheck --check-prefix=RETWARN --implicit-check-not="cannot parse" %s
// RETWARN: bundle root:
// RUN: rm -f %t.ret/cli.js %t.ret/dep.js
// RUN: %hermes-node --bundle=%t.ret/app.hbb | %FileCheck --check-prefix=RETEXEC %s
// RETEXEC: RETURN 2

// A file the *parser* rejects takes the same path as one the compiler
// rejects, and is reported against the parser. The two failures are found
// in different phases (the scan that collects require() calls, and the
// compile that follows the whole walk), so one test does not cover both.
// RUN: rm -rf %t.parse && mkdir -p %t.parse
// RUN: echo "function ( {" > %t.parse/broken.js
// RUN: echo "try { require('./broken.js'); } catch (e) { console.log('PARSE', e.name); } console.log('PARSE DONE');" > %t.parse/cli.js
// RUN: %hermes-node --build-bundle=%t.parse/app.hbb %t.parse/cli.js 2>&1 | %FileCheck --check-prefix=PARSEWARN %s
// PARSEWARN: warning: cannot parse {{.*}}broken.js ({{.*}}); packaged as a module that throws when required
// RUN: rm -f %t.parse/cli.js %t.parse/broken.js
// RUN: %hermes-node --bundle=%t.parse/app.hbb | %FileCheck --check-prefix=PARSEEXEC %s
// PARSEEXEC: PARSE SyntaxError
// PARSEEXEC: PARSE DONE

// The stub carries the compiler's own diagnostic, and names the file it
// stands for: a bundled module has no source on disk for a stack trace to
// point at, so the message is the only thing that can.
// RUN: rm -rf %t.msg && mkdir -p %t.msg
// RUN: echo "module.exports = function (f) { return import(f); };" > %t.msg/dyn.cjs
// RUN: echo "try { require('./dyn.cjs'); } catch (e) { console.log('MSG', e.message); }" > %t.msg/cli.js
// RUN: %hermes-node --build-bundle=%t.msg/app.hbb %t.msg/cli.js 2>&1 | %FileCheck --check-prefix=MSGWARN %s
// MSGWARN: warning: cannot compile
// RUN: %hermes-node --bundle=%t.msg/app.hbb | %FileCheck --check-prefix=MSG %s
// MSG: MSG {{.*}}dyn.cjs: {{[0-9]+}}:{{[0-9]+}}:Invalid expression encountered

// A stubbed module does not disturb the rest of the container: its
// neighbours are packaged and run normally, and the bundle is still
// self-contained with the tree gone.
// RUN: rm -rf %t.mixed && mkdir -p %t.mixed
// RUN: echo "module.exports = function (f) { return import(f); };" > %t.mixed/dyn.cjs
// RUN: echo "module.exports = { v: 41 };" > %t.mixed/ok.js
// RUN: echo "const ok = require('./ok'); try { require('./dyn.cjs'); } catch (e) {} console.log('MIXED', ok.v + 1);" > %t.mixed/cli.js
// RUN: %hermes-node --build-bundle=%t.mixed/app.hbb %t.mixed/cli.js 2>&1 | %FileCheck --check-prefix=MIXEDWARN %s
// MIXEDWARN: warning: cannot compile
// RUN: rm -f %t.mixed/cli.js %t.mixed/ok.js %t.mixed/dyn.cjs
// RUN: %hermes-node --bundle=%t.mixed/app.hbb | %FileCheck --check-prefix=MIXED %s
// MIXED: MIXED 42

// A bundle never reads code off the disk. A specifier the container cannot
// answer is an error naming the importer and the remedy, not a filesystem
// lookup -- which is both the point of shipping a bundle and the reason a
// computed specifier cannot be made to load arbitrary code.
// RUN: rm -rf %t.closed && mkdir -p %t.closed
// RUN: echo "module.exports = { v: 1 };" > %t.closed/ghost.js
// RUN: echo "const n = 'gh' + 'ost'; try { require('./' + n); } catch (e) { console.log('CLOSED', e.code); }" > %t.closed/cli.js
// RUN: %hermes-node --build-bundle=%t.closed/app.hbb %t.closed/cli.js
// RUN: %hermes-node --bundle=%t.closed/app.hbb | %FileCheck --check-prefix=CLOSED %s
// CLOSED: CLOSED MODULE_NOT_FOUND

// A .node native addon is the one specifier whose message is not "--include
// it": the producer skips .node files because they are not JavaScript and
// there is nothing to compile, so naming the flag would be advice that
// cannot work. The code stays MODULE_NOT_FOUND -- a probing caller branches
// on that and must still see the "no" it expects -- and only the text
// differs. Addons work unbundled (examples/bufferutil-addon); in a bundle
// they wait for a mechanism of their own.
// RUN: rm -rf %t.addon && mkdir -p %t.addon/native
// RUN: touch %t.addon/native/thing.node
// RUN: echo "try { require('./native/thing.node'); } catch (e) { console.log('ADDON', e.code, e.message.split('\\n').pop().trim()); }" > %t.addon/cli.js
// RUN: %hermes-node --build-bundle=%t.addon/app.hbb %t.addon/cli.js 2>&1 | %FileCheck --check-prefix=ADDONWARN %s
// ADDONWARN: warning: skipping {{.*}}thing.node (.node is not packageable)
// RUN: %hermes-node --bundle=%t.addon/app.hbb | %FileCheck --check-prefix=ADDON %s
// ADDON: ADDON MODULE_NOT_FOUND Native addons are not supported in a bundle yet.
