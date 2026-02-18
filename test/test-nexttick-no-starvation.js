// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS (nextTick drained in {{[0-9]+}}ms)
// Regression test: process.nextTick scheduled from a timer callback (which fires
// in libuv's "timers" phase) must drain promptly -- before the poll phase blocks.
//
// Without the uv_prepare_t tick drain handle, nextTick only drains in the
// uv_check_t (after poll). If a long-lived timer sets the poll timeout, the
// nextTick callback is starved for the duration of that timer.
'use strict';

// A long timer keeps the event loop alive and sets a large poll timeout.
// If nextTick is starved, the poll phase blocks for up to 5 seconds.
var watchdog = setTimeout(function() {
  console.log('FAIL: nextTick was starved for 5 seconds');
  process.exit(1);
}, 5000);

// A 0ms timer fires immediately in the "timers" phase, then schedules nextTick.
// With the prepare-phase drain, nextTick fires before poll (~0ms).
// Without it, nextTick waits until after poll (~5000ms).
setTimeout(function() {
  var start = Date.now();
  process.nextTick(function() {
    var elapsed = Date.now() - start;
    clearTimeout(watchdog);
    if (elapsed < 500) {
      console.log('PASS (nextTick drained in ' + elapsed + 'ms)');
    } else {
      console.log('FAIL: nextTick took ' + elapsed + 'ms, expected <500ms');
      process.exit(1);
    }
  });
}, 0);
