// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A container carries the VM options its program needs, and says so under
// --dump, so it can be audited before it ships.

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "console.log('PROXY', typeof Proxy);" > %t.tree/cli.js

// --vm= with --build-bundle records rather than applies.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --vm=-Xes6-proxy=false %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/app.hbb --dump | %FileCheck --check-prefix=DUMP %s
// DUMP: VM_OPTIONS (1)
// DUMP: overrides: locked
// DUMP: -Xes6-proxy=false
// The table is a section with real bytes, not a free extra.
// DUMP: vmopts {{[1-9][0-9]*}} B

// A container with no options dumps exactly as it did before this section
// existed.
// RUN: %hermes-node --build-bundle=%t.tree/plain.hbb %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/plain.hbb --dump | %FileCheck --check-prefix=PLAIN --implicit-check-not=VM_OPTIONS %s
// PLAIN: MODULES

// --vm is accepted alongside --build-bundle: it records rather than
// configures, so it is not one of the verbs checkToolOptions() refuses it
// with. All four RUN lines above already depend on that; this is the case
// that says so out loud.

// The most open container there is -- the bit set and nothing baked -- says
// so. It honours HERMES_NODE_VM_OPTIONS unconditionally, -enable-eval=true
// and -Xhermes-internal-test-methods=true included, so a --dump that said
// nothing about it (which is what an options-count-only condition did) hid
// the one thing an audit before shipping most needs to see.
// RUN: %hermes-node --build-bundle=%t.tree/openplain.hbb --allow-vm-options-override %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/openplain.hbb --dump | %FileCheck --check-prefix=OPENPLAIN %s
// OPENPLAIN: VM_OPTIONS (0)
// OPENPLAIN: overrides: allowed

// --allow-vm-options-override is recorded and shown.
// RUN: %hermes-node --build-bundle=%t.tree/open.hbb --vm=-Xes6-proxy=false --allow-vm-options-override %t.tree/cli.js
// RUN: %hermes-node --bundle=%t.tree/open.hbb --dump | %FileCheck --check-prefix=OPEN %s
// OPEN: overrides: allowed

// Running a container applies the options it recorded. (This assertion
// belongs to this task rather than Task 5: Task 5 records them, this task
// is what makes them take effect.)
//
// -Xes6-proxy is deliberately NOT reused for these cases, even though it
// drives every check above: turning it off does not merely make Proxy
// disappear, it takes Reflect with it (Hermes gates both behind the same
// runtime bit, GlobalObject.cpp), and libjs/primordials.js needs Reflect
// at bootstrap on every hermes-node invocation, bundled or not. So a
// container that actually applies "-Xes6-proxy=false" never reaches the
// point of printing anything -- it dies in primordials.js before its own
// entry module runs, for every one of the eight ways below that this
// section would otherwise have ended up with the option in effect. That
// is a property of Hermes's Proxy/Reflect coupling and this runtime's
// bootstrap order, not of the run-time application logic under test, so
// -enable-eval, which has no such coupling, carries the same eight cases
// instead. See task-6-report.md for the failure this replaced.
// RUN: echo "console.log('EVAL', (function(){try{eval('1+1');return 'works';}catch(e){return 'blocked';}})());" > %t.tree/eval.js
// RUN: %hermes-node --build-bundle=%t.tree/evalapp.hbb --vm=-enable-eval=false %t.tree/eval.js
// RUN: %hermes-node --build-bundle=%t.tree/evalopen.hbb --vm=-enable-eval=false --allow-vm-options-override %t.tree/eval.js

// RUN: %hermes-node --bundle=%t.tree/evalapp.hbb | %FileCheck --check-prefix=BLOCKED %s
// BLOCKED: EVAL blocked

// Locked is the default, and an override attempt is an error rather than a
// setting that quietly does not take effect.
// RUN: %not %hermes-node --vm=-enable-eval=true --bundle=%t.tree/evalapp.hbb 2>&1 | %FileCheck --check-prefix=LOCKED %s
// LOCKED: locked
// LOCKED: --allow-vm-options-override

// Including from the environment.
// RUN: env HERMES_NODE_VM_OPTIONS=-enable-eval=true %not %hermes-node --bundle=%t.tree/evalapp.hbb 2>&1 | %FileCheck --check-prefix=LOCKEDENV %s
// LOCKEDENV: HERMES_NODE_VM_OPTIONS

// A locked container with no baked options at all is still locked: locking
// is a property of the container (allowOverride, recorded whether or not
// there is anything to override), not a side effect of having options to
// protect.
// RUN: %not %hermes-node --vm=-enable-eval=true --bundle=%t.tree/plain.hbb 2>&1 | %FileCheck --check-prefix=LOCKED %s

// Both --vm= and HERMES_NODE_VM_OPTIONS at once: the error names the
// command line, since --vm= is checked first and either alone is already
// refused.
// RUN: env HERMES_NODE_VM_OPTIONS=-enable-eval=true %not %hermes-node --vm=-enable-eval=true --bundle=%t.tree/evalapp.hbb 2>&1 | %FileCheck --check-prefix=LOCKEDBOTH %s
// LOCKEDBOTH: --vm was given on the command line.

// With the bit set, both work, and the run-time value wins because it is
// appended after the container's.
// RUN: %hermes-node --vm=-enable-eval=true --bundle=%t.tree/evalopen.hbb | %FileCheck --check-prefix=WORKS %s
// WORKS: EVAL works
// RUN: env HERMES_NODE_VM_OPTIONS=-enable-eval=true %hermes-node --bundle=%t.tree/evalopen.hbb | %FileCheck --check-prefix=WORKS %s

// The command line beats the environment.
// RUN: env HERMES_NODE_VM_OPTIONS=-enable-eval=true %hermes-node --vm=-enable-eval=false --bundle=%t.tree/evalopen.hbb | %FileCheck --check-prefix=BLOCKED %s

// An unlocked container with no run-time override still applies its own.
// RUN: %hermes-node --bundle=%t.tree/evalopen.hbb | %FileCheck --check-prefix=BLOCKED %s
