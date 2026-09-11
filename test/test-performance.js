// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Test globalThis.performance and require('perf_hooks').
//
// The property most worth pinning is sub-millisecond resolution, because
// that is exactly what a naive Date.now()-based implementation fails: two
// Date.now() calls close together return the same whole millisecond far
// more often than not, and Date.now() differences can even go backwards if
// the wall clock is adjusted. performance.now() must instead be built on a
// monotonic, sub-millisecond clock (process.hrtime.bigint(), see
// libjs/shims/perf_hooks.js) -- so the assertions here are that repeated
// calls never go backwards, and that a fractional (non-integer)
// millisecond value shows up. Neither assertion depends on any specific
// duration, so it should not be flaky under parallel load.
//
// RUN: %hermes-node %s | %FileCheck %s

'use strict';

var failures = [];
function check(cond, msg) {
  if (!cond) failures.push(msg);
}

// --- globalThis.performance exists, with exactly the two members. ---
check(typeof performance === 'object' && performance !== null,
    'typeof performance');
check(typeof performance.now === 'function', 'typeof performance.now');
check(typeof performance.timeOrigin === 'number', 'typeof performance.timeOrigin');

// --- now() is monotonic (non-decreasing) across many calls. ---
var samples = [];
for (var i = 0; i < 2000; i++) {
  samples.push(performance.now());
}
var monotonic = true;
for (var i = 1; i < samples.length; i++) {
  if (samples[i] < samples[i - 1]) {
    monotonic = false;
    break;
  }
}
check(monotonic, 'monotonic');

// --- Sub-millisecond resolution: some sample is not a whole number. ---
// A millisecond-granular implementation (Date.now() differences) would
// make every sample an integer; a nanosecond-granular one essentially
// never does, so this is the discriminating assertion the design calls
// for. Checking many samples rather than one keeps this robust rather
// than relying on a single lucky (or unlucky) reading.
var sawFractional = false;
for (var i = 0; i < samples.length; i++) {
  if (samples[i] !== Math.floor(samples[i])) {
    sawFractional = true;
    break;
  }
}
check(sawFractional, 'sub-millisecond resolution');

// Also require two nearby calls to differ by a non-integer amount at least
// once, which is a slightly different angle on the same property (rules
// out an implementation that e.g. floors every individual sample but keeps
// a fractional internal accumulator that would show up in a delta).
var sawFractionalDelta = false;
for (var i = 1; i < samples.length; i++) {
  var delta = samples[i] - samples[i - 1];
  if (delta !== Math.floor(delta)) {
    sawFractionalDelta = true;
    break;
  }
}
check(sawFractionalDelta, 'sub-millisecond delta');

// --- timeOrigin is a plausible wall-clock time, close to Date.now(). ---
var nowWall = Date.now();
check(performance.timeOrigin > 1577836800000, // 2020-01-01, sanity floor
    'timeOrigin plausible (too small): ' + performance.timeOrigin);
check(performance.timeOrigin <= nowWall + 1000,
    'timeOrigin plausible (in the future): ' + performance.timeOrigin);
var estimatedNow = performance.timeOrigin + performance.now();
var diff = Math.abs(estimatedNow - nowWall);
// A tight bound here, deliberately -- this is NOT a "how long did the test
// take" measurement that a loaded 16-way parallel suite could blow up.
// timeOrigin and performance.now() both advance in real time between the
// moment they are captured (at bootstrap, and a few lines above) and the
// moment Date.now() is read just above, and they advance together: any
// scheduling delay between "the process started" and "this line runs"
// lands in both `estimatedNow` and `nowWall` equally and cancels out of
// their difference. What does NOT cancel out is a bug in how now() and
// timeOrigin are related to each other -- e.g. now() measured from when
// the perf_hooks shim's module body ran (well into bootstrap) rather than
// from timeOrigin (process start), which was exactly this project's own
// bug: an ASAN build's slower startup made now() start ~200ms short of
// where timeOrigin says it should. 100ms comfortably covers real
// scheduling noise between the two adjacent statements above while still
// catching that class of bug by two orders of magnitude.
check(diff < 100,
    'timeOrigin + now() tracks Date.now(): diff=' + diff);

// --- performance is writable and configurable on globalThis. ---
var desc = Object.getOwnPropertyDescriptor(globalThis, 'performance');
check(!!desc && desc.writable === true, 'performance writable');
check(!!desc && desc.configurable === true, 'performance configurable');

// --- require('perf_hooks') and require('node:perf_hooks'). ---
var perfHooks = require('perf_hooks');
check(perfHooks.performance === globalThis.performance,
    'perf_hooks.performance === globalThis.performance');

var perfHooksScheme = require('node:perf_hooks');
check(perfHooksScheme.performance === globalThis.performance,
    'node:perf_hooks perf_hooks.performance === globalThis.performance');

// --- PerformanceObserver is absent -- deliberately, see the shim's own
// comment for why a stub would be worse than nothing. ---
check(perfHooks.PerformanceObserver === undefined,
    'PerformanceObserver absent from perf_hooks');
check(typeof globalThis.PerformanceObserver === 'undefined',
    'PerformanceObserver absent from globalThis');

// --- Nothing else is exported from the shim. ---
var exportedKeys = Object.keys(perfHooks);
check(exportedKeys.length === 1 && exportedKeys[0] === 'performance',
    'perf_hooks exports only performance: ' + JSON.stringify(exportedKeys));

if (failures.length === 0) {
  print('PASS');
} else {
  print('FAIL: ' + failures.join(', '));
}

// CHECK: PASS
