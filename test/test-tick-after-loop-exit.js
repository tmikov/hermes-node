// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test: a tick scheduled from the event loop's last callback still runs
// before the process exits.

'use strict';

var ticked = false;
var microtaskRan = false;

// This timer is the only thing keeping the loop alive, so uv_run() returns as
// soon as it has fired. Deliberately no output from here: writing to stdout
// would keep the loop alive for another turn and hide the drop.
setTimeout(function () {
  process.nextTick(function () {
    ticked = true;
  });
  Promise.resolve().then(function () {
    microtaskRan = true;
  });
}, 10);

process.on('exit', function () {
  if (!ticked) {
    console.log('FAIL: tick scheduled from the last timer was dropped');
  } else if (!microtaskRan) {
    console.log('FAIL: microtask scheduled from the last timer was dropped');
  } else {
    console.log('PASS');
  }
});
