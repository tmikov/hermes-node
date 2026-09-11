// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Shim for perf_hooks.
//
// Node's real perf_hooks.js (libjs-node/perf_hooks.js) and the ten files
// under libjs-node/internal/perf/ all need internalBinding('performance'),
// which does not exist here -- it is a native binding that would provide
// `now`, `milestones`, the NODE_PERFORMANCE_MILESTONE_* constants,
// `getMilestoneTimestamp`, `loopIdleTime`, `createELDHistogram` (a native
// histogram) and observer plumbing. That is a feature of its own, not this
// shim. libjs/shims/internal/perf/observe.js is the existing precedent: a
// small stub written for the same missing binding, used by dns.js.
//
// So this shim exports exactly one thing: `performance`, with exactly two
// members, `now()` and `timeOrigin`. No PerformanceObserver, no
// monitorEventLoopDelay, no constants, no createHistogram. This is
// deliberate, and it is the part most likely to look incomplete and invite
// "helpfully" widening it -- don't. A library that checks
// `typeof PerformanceObserver === 'function'` needs the true answer: an
// inert stub would report the class as present and let the library believe
// it is observing performance entries, when nothing would ever be recorded.
// An absent global is honest and detectable; a present-but-inert one is a
// silent lie that only shows up as "why did my observer never fire".
//
// `now()` and `timeOrigin` are implemented on top of process.hrtime and
// process.uptime rather than internalBinding('performance') or a native
// binding of our own:
//
//  - process.hrtime.bigint() is backed by uv_hrtime() (see
//    lib/process/node_process.cpp), a monotonic clock with sub-millisecond
//    (nanosecond) resolution, so a difference of two calls can never go
//    backwards even if the wall clock is stepped or slewed -- a property
//    Date.now() differences cannot offer, since Date.now() is
//    millisecond-granular and tracks the wall clock.
//  - process.uptime() is measured from the same uv_hrtime() clock, from the
//    moment the native NodeProcess object is constructed
//    (NodeProcess::NodeProcess() : startTime_(uv_hrtime()) {} in
//    lib/process/node_process.cpp), which happens at process start, before
//    any user script runs. So `Date.now() - process.uptime() * 1000` is a
//    good estimate of wall-clock time at process start, which is what
//    `timeOrigin` is defined to be.
//
// `now()` and `timeOrigin` must share the same origin (process start) or
// they do not compose: `timeOrigin + now()` is supposed to track
// `Date.now()`, exactly as it does in Node, where `performance.now()` on
// the very first call already reads a few milliseconds, not ~0. This
// module itself loads well after process start -- bootstrap has already
// run the native runtime, event loop and napi_env setup by the time this
// shim's top level executes -- so an `originHr` captured only here, with
// no correction, would make every `now()` value short by however long
// that bootstrap took (tens of milliseconds in a Release build, and
// enough to be obvious -- ~200 ms -- in an ASAN build's slower startup).
// `process.uptime()`, read once at this same moment and folded in as a
// starting offset, closes exactly that gap: `now()` becomes "milliseconds
// since process start", matching `timeOrigin`'s own definition, rather
// than "milliseconds since this module loaded".

'use strict';

// Both captured once, at module load time (which for this shim is during
// bootstrap, since globalThis.performance is installed from it -- see
// hermes_node_runtime.cpp), from the same moment: `uptimeAtBootstrapMs` is
// how much of that gap now() must account for, and `originHr` is the hrtime
// reading every now() call measures its own delta from.
var uptimeAtBootstrapMs = process.uptime() * 1000;
var originHr = process.hrtime.bigint();
var timeOrigin = Date.now() - uptimeAtBootstrapMs;

var performance = {
  // Milliseconds since timeOrigin (i.e. since process start), as a double
  // with sub-millisecond resolution -- matching the Performance
  // interface's `now()`, which the spec defines to return a
  // DOMHighResTimeStamp (a double number of milliseconds, not an integer).
  // `uptimeAtBootstrapMs` carries the offset from timeOrigin (process
  // start) to this module's own load time; the hrtime delta covers
  // everything since.
  now: function now() {
    return uptimeAtBootstrapMs +
        Number(process.hrtime.bigint() - originHr) / 1e6;
  },
  timeOrigin: timeOrigin,
};

module.exports = { performance: performance };
