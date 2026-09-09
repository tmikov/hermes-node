// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The `cache` subcommand: hermes-node's only subcommand, dispatched before
// the ordinary parse loop so that the invariant that loop rests on -- every
// argument after the first positional belongs to the program being run --
// is left exactly as it was.
//
// This file is never executed as a script; it is here for the RUN lines.

// A cache the runtime populated, so info describes something real.
// RUN: rm -rf %t.cache %t.dir && mkdir -p %t.dir
// RUN: echo 'module.exports = 7;' > %t.dir/dep.js
// RUN: echo 'require(process.argv[2]); console.log("ran");' > %t.dir/app.js
// RUN: %hermes-node-cc --compile-cache=%t.cache %t.dir/app.js %t.dir/dep.js | %FileCheck --check-prefix=RAN %s

// info names the root, the configuration and the current generation. The
// current generation is listed even when it holds nothing, because "which
// generation am I on" is the question this command exists to answer.
// RUN: %hermes-node cache info --compile-cache=%t.cache | %FileCheck --check-prefix=INFO %s

// An absent cache is not an error: never having compiled anything is a
// normal state, and exit 1 would make a status check out of it.
// RUN: %hermes-node cache info --compile-cache=%t.absent | %FileCheck --check-prefix=ABSENT %s

// prune reports what it did, and says so plainly when there was nothing to
// do rather than printing a table of zeroes.
// RUN: %hermes-node cache prune --compile-cache=%t.cache | %FileCheck --check-prefix=PRUNE %s

// clean removes the whole tree, configuration file included, and a second
// clean has nothing left to remove.
// RUN: %hermes-node cache clean --compile-cache=%t.cache | %FileCheck --check-prefix=CLEAN %s
// RUN: %hermes-node cache clean --compile-cache=%t.cache | %FileCheck --check-prefix=NOTHING %s
// RUN: %hermes-node cache info --compile-cache=%t.cache | %FileCheck --check-prefix=ABSENT %s

// clean --generation leaves the configuration file and any other generation.
// RUN: rm -rf %t.g && %hermes-node-cc --compile-cache=%t.g %t.dir/app.js %t.dir/dep.js > /dev/null
// RUN: %hermes-node cache clean --generation --compile-cache=%t.g > /dev/null
// RUN: test -f %t.g/config
// RUN: %hermes-node cache info --compile-cache=%t.g | %FileCheck --check-prefix=KEPTCONFIG %s

// Errors name what was wrong. An unknown action, a second action, an option
// belonging to another action, and no action at all.
// RUN: %not %hermes-node cache nonsense 2>&1 | %FileCheck --check-prefix=EBADACTION %s
// RUN: %not %hermes-node cache info prune 2>&1 | %FileCheck --check-prefix=ETWO %s
// RUN: %not %hermes-node cache info --generation 2>&1 | %FileCheck --check-prefix=EGEN %s
// RUN: %not %hermes-node cache 2>&1 | %FileCheck --check-prefix=ENOACTION %s
// RUN: %not %hermes-node cache info --nonsense 2>&1 | %FileCheck --check-prefix=EOPT %s
// RUN: %not %hermes-node cache info --compile-cache= 2>&1 | %FileCheck --check-prefix=EEMPTY %s

// The subcommand has its own help.
// RUN: %hermes-node cache --help | %FileCheck --check-prefix=HELP %s

// It runs no runtime at all, like the other tool verbs: pointed at a
// directory it may not create one, and it may not execute a script.
// RUN: rm -rf %t.untouched
// RUN: %hermes-node cache info --compile-cache=%t.untouched > /dev/null
// RUN: test ! -e %t.untouched

// RAN: ran
// INFO: compile cache: {{.*}}
// INFO: config:
// INFO: recency:
// INFO: max_wasm_bytes:
// INFO: generations:
// INFO: (current)
// INFO: total:
// ABSENT: (does not exist)
// PRUNE: compile cache: {{.*}}
// CLEAN: removed the cache
// NOTHING: nothing to clean
// KEPTCONFIG: max_wasm_bytes:
// EBADACTION: unknown 'cache' action 'nonsense'
// ETWO: 'cache' takes one action, got 'info' and 'prune'
// EGEN: --generation applies to 'cache clean', not 'info'
// ENOACTION: 'cache' requires an action
// EOPT: unknown option '--nonsense' for 'cache'
// EEMPTY: --compile-cache= requires a directory
// HELP: Usage: {{.*}} cache <action> [options]
// HELP: To run a script named
