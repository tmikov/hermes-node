// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// process.emit() returns whether the event had listeners, as an
// EventEmitter's emit() does -- false once the last one has been removed,
// not only when none was ever added. Two callers decide from that answer
// whether a listener took an error: process._fatalException
// (libjs/process-events.js) for an uncaught exception, and
// emitUnhandledRejection (libjs-node/internal/process/promises.js) for an
// unhandled rejection. A wrong `true` swallowed every such error after a
// process.once() listener for it had fired: the process carried on and
// exited 0.
//
// Two runs, selected by the first argument; each status is exactly
// node v24.13.1's (1).
//
// `exception`: EMIT, the answers after on, then off; once, then emitted
// twice; removeAllListeners (node's answers). Then two throwing timers
// after a once('uncaughtException') listener: the listener takes the
// first, the second is reported and the process exits 1, and the timer
// after it never runs.
//
// `rejection`: two unhandled rejections after a once('unhandledRejection')
// listener. Node's default mode is `throw` (getUnhandledRejectionsMode,
// lib/internal/process/promises.js): throwUnhandledRejectionsMode emits
// 'unhandledRejection' and, when nothing handled it, calls
// triggerUncaughtException(err, true), so the listener takes the first
// and the second ends the process with status 1. That much holds here.
// Two differences from node that the emit fix does not govern, not
// asserted, and reported to the tracker through the progress file:
//  - node reports the Error itself; here it is wrapped in
//    UnhandledPromiseRejection, because isErrorLike() requires an own
//    `stack` property and Hermes defines `stack` as an accessor on
//    Error.prototype. The check accepts either form, both naming `second`.
//  - node handles each rejection on the turn it happens; here the first
//    listener ran only after a 200 ms timer had fired. So this run has no
//    "still alive" timer: here it would print, under node it would not.
//
// RUN: %hermes-node %s exception > %t.exception 2>&1; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --implicit-check-not='STILL ALIVE' --check-prefix=EXCEPTION %s < %t.exception
// RUN: %hermes-node %s rejection > %t.rejection 2>&1; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --implicit-check-not='LISTENER second' --check-prefix=REJECTION %s < %t.rejection
//
// CODE: EXITCODE 1
//
// EXCEPTION: EMIT true false true false false
// EXCEPTION: LISTENER first
// EXCEPTION: Error: second
//
// REJECTION: LISTENER first
// REJECTION: {{Error: second|reason "Error: second"}}

'use strict';

const fs = require('fs');
const say = (s) => fs.writeSync(2, s + '\n');

if (process.argv[2] === 'rejection') {
  process.once('unhandledRejection', (r) => say('LISTENER ' + r.message));
  Promise.reject(new Error('first'));
  setTimeout(() => {
    Promise.reject(new Error('second'));
  }, 50);
} else {
  function f() {}
  const answers = [];
  process.on('a', f);
  answers.push(process.emit('a'));
  process.off('a', f);
  answers.push(process.emit('a'));
  process.once('b', f);
  answers.push(process.emit('b'));
  answers.push(process.emit('b'));
  process.on('c', f);
  process.removeAllListeners('c');
  answers.push(process.emit('c'));
  say('EMIT ' + answers.join(' '));

  process.once('uncaughtException', (e) => say('LISTENER ' + e.message));
  setTimeout(() => {
    throw new Error('first');
  }, 10);
  setTimeout(() => {
    throw new Error('second');
  }, 50);
  setTimeout(() => say('STILL ALIVE'), 200);
}
