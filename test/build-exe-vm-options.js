// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A linked executable carries its container's VM options. It has no
// command line of its own -- every argument belongs to the program -- so
// HERMES_NODE_VM_OPTIONS is the only way to reach it, and only when the
// container was built to allow it.
//
// -enable-eval, not -Xes6-proxy: turning Proxy off takes Reflect with it
// (Hermes gates both behind the same runtime bit), and libjs/primordials.js
// needs Reflect at bootstrap, so an executable that actually applied
// -Xes6-proxy=false would die before printing anything. -enable-eval has no
// such coupling. See test/bundle-vm-options.js for the same reasoning.
//
// REQUIRES: linker-available

// RUN: rm -rf %t.tree && mkdir -p %t.tree
// RUN: echo "console.log('EVAL', (function(){try{eval('1+1');return 'works';}catch(e){return 'blocked';}})());" > %t.tree/cli.js

// Locked.
// RUN: %hermes-node --build-bundle=%t.tree/app.hbb --vm=-enable-eval=false %t.tree/cli.js
// RUN: %hermes-node --build-exe=%t.tree/app --kit=%kit_dir %t.tree/app.hbb
// RUN: %t.tree/app | %FileCheck --check-prefix=BLOCKED %s
// BLOCKED: EVAL blocked

// The environment cannot open a locked artifact.
// The remediation is two steps here where it is one for a container, and
// that second line is the only thing distinguishing this message from the
// bundle one -- so it is the half worth asserting.
// RUN: env HERMES_NODE_VM_OPTIONS=-enable-eval=true %not %t.tree/app 2>&1 | %FileCheck --check-prefix=LOCKED %s
// LOCKED: locked
// LOCKED: --build-bundle --allow-vm-options-override
// LOCKED: then link it again with: --build-exe

// Unlocked.
// RUN: %hermes-node --build-bundle=%t.tree/open.hbb --vm=-enable-eval=false --allow-vm-options-override %t.tree/cli.js
// RUN: %hermes-node --build-exe=%t.tree/open --kit=%kit_dir %t.tree/open.hbb
// RUN: %t.tree/open | %FileCheck --check-prefix=BLOCKED %s
// RUN: env HERMES_NODE_VM_OPTIONS=-enable-eval=true %t.tree/open | %FileCheck --check-prefix=WORKS %s
// WORKS: EVAL works
