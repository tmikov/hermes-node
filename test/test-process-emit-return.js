// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// process.emit() returns whether the event had listeners, as an
// EventEmitter's emit() does -- false once the last one has been removed,
// not only when none was ever added. process._fatalException
// (libjs/process-events.js) decides from that answer whether a listener
// took an uncaught exception, so a wrong `true` swallowed every uncaught
// exception after a process.once('uncaughtException') listener had
// handled one: the process carried on and exited 0.
//
// EMIT: on, then off; once, then emitted twice; removeAllListeners. The
// answers are node v24.13.1's. Then two throwing timers after a once
// listener: the listener takes the first, the second is reported and the
// process exits 1 (%not), as under Node, and the timer after it never
// runs.
//
// RUN: %not %hermes-node %s 2>&1 | %FileCheck --implicit-check-not='STILL ALIVE' %s
// CHECK: EMIT true false true false false
// CHECK: LISTENER first
// CHECK: Error: second

'use strict';

const fs = require('fs');
const say = (s) => fs.writeSync(2, s + '\n');

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
