// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --vm= reaches the Hermes VM. Every flag asserted here is observable from
// JavaScript with no timing dependency, so these cases test effect rather
// than merely that a flag parsed.
//
// -Xes6-proxy=false was tried here first, since "typeof Proxy" is the most
// obvious way to show a flag took effect. It does not work: Hermes gates
// the global Reflect object on the same hasES6Proxy() check as Proxy
// itself (hermes/lib/VM/JSLib/GlobalObject.cpp, both under "if
// (runtime.hasES6Proxy())"), and libjs/primordials.js unconditionally
// destructures Reflect during bootstrap -- before any user script runs.
// So that flag does not fail *this test's* source, it fails hermes-node's
// own startup, for every program. -enable-eval=false is the replacement:
// it is honoured (kHonoured in lib/vm-options/vm_options.cpp), defaults to
// true, and its effect is confined to a JS-level eval() call, which the
// bootstrap never makes.

// A flag takes effect.
// RUN: echo "console.log('EVAL', (function(){ try { eval('1'); return 'ok'; } catch (e) { return 'blocked'; } })());" > %t.eval.js
// RUN: %hermes-node --vm=-enable-eval=false %t.eval.js | %FileCheck --check-prefix=NOEVAL %s
// NOEVAL: EVAL blocked

// Without the flag it is there.
// RUN: %hermes-node %t.eval.js | %FileCheck --check-prefix=EVAL %s
// EVAL: EVAL ok

// hermes-node's own defaults survive the introduction of the flag
// machinery. Hermes defaults -Xasync-generators to false; hermes-node
// forces it on, and this is the case that fails if that is ever lost.
// RUN: echo "async function* g() { yield 1; } g().next().then(v => console.log('ASYNCGEN', v.value));" > %t.agen.js
// RUN: %hermes-node %t.agen.js | %FileCheck --check-prefix=AGEN %s
// AGEN: ASYNCGEN 1

// And it can still be turned off explicitly -- through eval(), not a
// top-level script. hermes-node compiles a top-level script through
// Hermes's NAPI compile path (hermes_napi_compile.cpp / hermes_napi.cpp),
// which hardcodes compileFlags.enableAsyncGenerators = true regardless of
// RuntimeConfig, so -Xasync-generators has no observable effect there
// (an earlier version of this test tried exactly that vector and found
// no distinguishable failure). eval() is different: Hermes's own
// JSLib/eval.cpp sets compileFlags.enableAsyncGenerators from
// runtime.hasAsyncGenerators() -- the live RuntimeConfig field --
// so this is where -Xasync-generators=false actually reaches.
// RUN: %not %hermes-node --vm=-Xasync-generators=false -e "eval('async function* g(){ yield 1; }')" 2>&1 | %FileCheck --check-prefix=NOAGEN %s
// NOAGEN: SyntaxError
// NOAGEN: async generators are unsupported

// And by default (no --vm=) the same eval() call succeeds, same as AGEN
// above at the top level.
// RUN: %hermes-node -e "eval('async function* g(){ yield 1; }'); console.log('AGENEVAL', 'ok');" | %FileCheck --check-prefix=AGENEVAL %s
// AGENEVAL: AGENEVAL ok

// Last occurrence wins. buildVmRuntimeConfig deduplicates to make this
// true: a plain llvh::cl::opt is cl::Optional and rejects a second
// occurrence outright, so a repeat must not reach the parser twice.
// RUN: %hermes-node --vm=-enable-eval=false --vm=-enable-eval=true %t.eval.js | %FileCheck --check-prefix=EVAL %s

// HERMES_NODE_VM_OPTIONS reaches a plain script, not only a container run.
// It was ignored here until 2026-09-02, which made it a silent no-op on a
// VM setting -- the failure shape the rest of this feature exists to
// refuse.
// RUN: env HERMES_NODE_VM_OPTIONS=-enable-eval=false %hermes-node %t.eval.js | %FileCheck --check-prefix=NOEVAL %s

// And --vm= still wins over it, the same precedence a container run gives
// the two: the environment is applied first, the command line after.
// RUN: env HERMES_NODE_VM_OPTIONS=-enable-eval=false %hermes-node --vm=-enable-eval=true %t.eval.js | %FileCheck --check-prefix=EVAL %s

// A memory size with a suffix parses, because Hermes's own parser handles
// it rather than a hand-rolled one.
// RUN: echo "console.log('RAN');" > %t.ok.js
// RUN: %hermes-node --vm=-gc-max-heap=512m %t.ok.js | %FileCheck --check-prefix=RAN %s
// RAN: RAN

// --vm-help lists the honoured flags and exits 0.
// RUN: %hermes-node --vm-help | %FileCheck --check-prefix=HELP %s
// HELP-DAG: -gc-max-heap
// HELP-DAG: -Xjit
// HELP-DAG: -Xes6-block-scoping
