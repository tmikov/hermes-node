// REQUIRES: linker-available, shermes-available
//
// Task 17. The native producer's shermes/cc failure policy used to be
// "every failure is a hard build error", justified by a claim that turned
// out to be false: the only tolerated case, `import()` inside a `.cjs`,
// was said to be a PARSE error the shared scanner already catches, so both
// producers would stub it identically. Measured, that is wrong -- the
// scanner accepts the file; it is IRGen, which only the compile step
// reaches, that rejects it. @babel/core ships exactly this construct
// (lib/config/files/import.cjs), so this was not a synthetic case: it is
// what stopped `build-native` from compiling most real Babel-based
// programs. See dz 01a0a0d6-03d4 and the "Failure policy" section of
// docs/superpowers/specs/2026-09-13-native-compilation-design.md.
//
// The first case below mirrors the bytecode producer's own tolerance for
// this file: a non-entry, non-preload module that fails to compile is
// packaged as a module that throws when required, with the same warning
// wording, and the build succeeds. The second case proves the fix did not
// overshoot: a compiler that fails for a reason that has nothing to do
// with the source (--cc=%false, which exits nonzero with no diagnostic
// text at all) still fails the whole build rather than silently stubbing
// everything.

// --- Case 1: a source rejection is tolerated, and the two producers agree.
//
// RUN: rm -rf %t.ok && mkdir -p %t.ok/src
// RUN: echo 'module.exports = function (f) { return import(f); };' > %t.ok/src/dyn.cjs
// RUN: echo 'let m;' > %t.ok/src/app.js
// RUN: echo 'try { m = require("./dyn.cjs"); } catch (e) { m = "threw"; console.error("STUB-MESSAGE: " + e.message); }' >> %t.ok/src/app.js
// RUN: echo 'console.log("LOADED", typeof m);' >> %t.ok/src/app.js
//
// The bytecode path: builds with a warning, and requiring the bad module
// throws rather than failing the whole program.
// RUN: %hermes-node --build-bundle=%t.ok/app.hbb %t.ok/src/app.js 2>%t.ok/bc-warn.txt | %FileCheck --check-prefix=BCSUMMARY %s
// RUN: %FileCheck --check-prefix=WARN %s < %t.ok/bc-warn.txt
// RUN: %hermes-node --bundle=%t.ok/app.hbb >%t.ok/bc-run.txt 2>%t.ok/bc-msg.txt
// RUN: %FileCheck --check-prefix=RUNS %s < %t.ok/bc-run.txt
//
// The native path: same warning wording, the BUILD SUCCEEDS, and the
// produced executable RUNS -- this is the property the design lost.
// RUN: %hermes-node build-native %t.ok/src/app.js -o %t.ok/app --kit=%kit_dir 2>%t.ok/nat-warn.txt | %FileCheck --check-prefix=NATSUMMARY %s
// RUN: %FileCheck --check-prefix=WARN %s < %t.ok/nat-warn.txt
// RUN: %t.ok/app >%t.ok/nat-run.txt 2>%t.ok/nat-msg.txt
// RUN: %FileCheck --check-prefix=RUNS %s < %t.ok/nat-run.txt
//
// Same program, two producers: stdout must agree byte for byte, which is
// the property the original design promised and Task 17 restores.
// RUN: diff -u %t.ok/bc-run.txt %t.ok/nat-run.txt
//
// The stub's own message (e.message, printed above to stderr rather than
// diffed against stdout) is the one place the two producers are NOT
// expected to agree, and is checked by shape rather than by equality.
// makeThrowingStub (bundle_build_internal.h) is shared code, but its two
// callers hand it different arguments: the bytecode path
// (bundle_build.cpp) passes the module's absolute path and
// takeCompileErrorText()'s tidy, one-line exception text; the native path
// (bundle_build_native.cpp) passes the module's relative container
// identity and shermes's whole captured stderr, caret lines included. Both
// still name the same underlying complaint, which is the substantive
// property worth pinning; the surrounding shape differs by design and is
// asserted as differing, not glossed over.
// RUN: %FileCheck --check-prefix=BCMSG %s < %t.ok/bc-msg.txt
// RUN: %FileCheck --check-prefix=NATMSG %s < %t.ok/nat-msg.txt
//
// The line-count difference is the shape assertion: one tidy line for the
// bytecode message against several for the native one, which is where the
// captured shermes diagnostic -- including its caret-and-tildes line --
// lands.
// RUN: test $(wc -l < %t.ok/bc-msg.txt) -eq 1
// RUN: test $(wc -l < %t.ok/nat-msg.txt) -gt 1
//
// WARN: warning: cannot compile
// WARN: Invalid expression encountered
// WARN: packaged as a module that throws when required
// RUNS: LOADED string
// BCSUMMARY: 1 packaged as throwing stub
// NATSUMMARY: 1 packaged as throwing stub
//
// The bytecode stub's message is one line, naming the absolute path.
// BCMSG: STUB-MESSAGE: {{.*}}/src/dyn.cjs: {{.*}}Invalid expression encountered
//
// The native stub's message names the relative identity, not the absolute
// path, and carries shermes's own multi-line diagnostic underneath it --
// structurally unlike the bytecode message above even though both are
// reporting the identical source defect. The tildes are shermes's own
// caret-diagnostic underline for the rejected `import(f)` expression.
// NATMSG: STUB-MESSAGE: dyn.cjs: dyn.cjs
// NATMSG: Invalid expression encountered
// NATMSG: ~~~~~

// --- Case 2: a hard toolchain failure stays hard, not stubbed away.
//
// helper.js is ordinary, parseable JavaScript with nothing wrong with it --
// %false exits nonzero and prints nothing, so this is "Exited, non-zero, no
// diagnostics", which must never be classified as a source rejection.
// RUN: rm -rf %t.hard && mkdir -p %t.hard/src
// RUN: echo 'module.exports = require("./helper.js")();' > %t.hard/src/app.js
// RUN: echo 'module.exports = function () { return 1; };' > %t.hard/src/helper.js
// RUN: %not %hermes-node build-native %t.hard/src/app.js -o %t.hard/app --kit=%kit_dir --cc=%false 2>%t.hard/err.txt
// RUN: %FileCheck --check-prefix=HARDFAIL %s < %t.hard/err.txt
// RUN: test ! -e %t.hard/app
//
// HARDFAIL: error:
// HARDFAIL-NOT: packaged as a module that throws
// HARDFAIL-NOT: wrote {{.*}} bytes

// This file is a lit driver only; the RUN lines above are the test.
console.log('unused');
