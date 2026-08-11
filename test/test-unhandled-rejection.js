// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s handled | %FileCheck --check-prefix=HANDLED %s
// RUN: %not %hermes-node %s 2>&1 | %FileCheck %s
// HANDLED: PASS
// CHECK: boom-unhandled-rejection
// Test: an unhandled promise rejection is reported and kills the process,
// while a handled one is left alone.

'use strict';

async function fail() {
  throw new Error('boom-unhandled-rejection');
}

if (process.argv[2] === 'handled') {
  // A rejection with a handler must not be reported and must not affect the
  // exit code.
  fail().catch(function (e) {
    if (e.message !== 'boom-unhandled-rejection') {
      throw new Error('wrong error reached the handler');
    }
    console.log('PASS');
  });
} else {
  // Nothing ever handles this one: hermes-node must print it and exit
  // non-zero rather than exiting 0 as if nothing happened.
  fail();
}
