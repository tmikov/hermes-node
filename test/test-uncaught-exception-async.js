// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s | %FileCheck %s

// An exception that escapes an asynchronous callback has to reach
// process._fatalException, the way node::errors::TriggerUncaughtException
// routes it. Two things follow from that and neither used to happen for a
// callback scheduled by a timer, an immediate or a tick: the
// 'uncaughtException' listeners run, and when none of them takes
// responsibility the process exits 1.
//
// Both halves were wrong in the same place. The native callback printed the
// error and returned, so a listener never saw it and the status stayed 0 --
// a test runner whose assertion failed inside a setTimeout exited 0 and
// looked green, which is the same failure mode process.exitCode had.
//
// A third symptom came from the same line: timers share one libuv handle,
// and the early return skipped the code that re-arms it, so every timer
// still pending after the throw silently stopped firing.
//
// Each expectation below is the status and output the same program produces
// under node v24.13.1.

var spawnSync = require('child_process').spawnSync;

var THROW = 'function f(){ throw new Error("BOOM"); }';
var HANDLER =
  'process.on("uncaughtException", function (e) {' +
  '  console.log("HANDLED " + e.message);' +
  '});';

var cases = [
  // --- Unhandled: the process must fail. ---
  ['setTimeout, no handler', THROW + 'setTimeout(f,1)', 1, null],
  ['setInterval, no handler', THROW + 'setInterval(f,1)', 1, null],
  ['setImmediate, no handler', THROW + 'setImmediate(f)', 1, null],
  ['nextTick, no handler', THROW + 'process.nextTick(f)', 1, null],
  // Promise rejection already routed correctly; here as a regression guard.
  ['promise rejection, no handler', THROW + 'Promise.resolve().then(f)', 1, null],

  // --- Handled: a listener takes it, so the program carries on. ---
  ['setTimeout, handled', HANDLER + THROW + 'setTimeout(f,1)', 0, 'HANDLED BOOM'],
  ['setImmediate, handled', HANDLER + THROW + 'setImmediate(f)', 0, 'HANDLED BOOM'],
  ['nextTick, handled', HANDLER + THROW + 'process.nextTick(f)', 0, 'HANDLED BOOM'],

  // A handled exception must not stop the timer machinery. The shared libuv
  // handle has to be re-armed, or this second timer never fires.
  ['a later timer still runs after a handled throw',
   HANDLER + THROW + 'setTimeout(f,1);' +
   'setTimeout(function(){console.log("LATER")},30)', 0, 'LATER'],

  // The listener decides the status, exactly as it does for a synchronous
  // throw it catches.
  ['a handler may still set a failing status',
   'process.on("uncaughtException",function(){process.exitCode=7});' +
   THROW + 'setTimeout(f,1)', 7, null],

  // --- The fatal path is still an orderly shutdown. ---
  ["'exit' still fires when nothing handles it",
   'process.on("exit",function(){console.log("EXITED")});' +
   THROW + 'setTimeout(f,1)', 1, 'EXITED'],

  // Output already written must survive the exit, or the diagnostic that
  // explains the failure is lost with it.
  ['output before the throw is not swallowed',
   'console.log("BEFORE");' + THROW + 'setTimeout(f,1)', 1, 'BEFORE'],

  // The error itself has to be reported; a silent exit 1 is not much better
  // than a silent exit 0.
  ['the error is reported on stderr', THROW + 'setTimeout(f,1)', 1, null, 'BOOM'],
];

var failed = 0;
for (var i = 0; i < cases.length; i++) {
  var name = cases[i][0];
  var code = cases[i][1];
  var wantStatus = cases[i][2];
  var wantOut = cases[i][3];
  var wantErr = cases[i][4];

  var r = spawnSync(process.execPath, ['-e', code], {encoding: 'utf8'});

  if (r.status !== wantStatus) {
    console.log('FAIL: ' + name + ': expected status ' + wantStatus +
                ', got ' + r.status);
    failed++;
  }
  if (wantOut !== null && wantOut !== undefined &&
      String(r.stdout).indexOf(wantOut) === -1) {
    console.log('FAIL: ' + name + ': stdout missing ' + JSON.stringify(wantOut) +
                ', got ' + JSON.stringify(String(r.stdout)));
    failed++;
  }
  if (wantErr !== undefined && String(r.stderr).indexOf(wantErr) === -1) {
    console.log('FAIL: ' + name + ': stderr missing ' + JSON.stringify(wantErr) +
                ', got ' + JSON.stringify(String(r.stderr)));
    failed++;
  }
}

if (failed === 0) {
  console.log('PASS');
}
// CHECK: PASS
