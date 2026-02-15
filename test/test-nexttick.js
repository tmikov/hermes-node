// Test for process.nextTick (task_queue binding + internal/process/task_queues).
'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var passed = 0;
var failed = 0;
var check = function(cond, msg) {
  if (cond) {
    passed++;
  } else {
    failed++;
    console.error('FAIL:', msg);
  }
};

// --- task_queue binding exports ---
var tq = internalBinding('task_queue');
check(typeof tq.tickInfo === 'object', 'tickInfo exists');
check(tq.tickInfo instanceof Uint32Array, 'tickInfo is Uint32Array');
check(tq.tickInfo.length === 2, 'tickInfo has 2 elements');
check(typeof tq.runMicrotasks === 'function', 'runMicrotasks is a function');
check(typeof tq.setTickCallback === 'function', 'setTickCallback is a function');
check(typeof tq.enqueueMicrotask === 'function', 'enqueueMicrotask is a function');
check(typeof tq.setPromiseRejectCallback === 'function', 'setPromiseRejectCallback is a function');
check(typeof tq.promiseRejectEvents === 'object', 'promiseRejectEvents is an object');
check(tq.promiseRejectEvents.kPromiseRejectWithNoHandler === 0, 'kPromiseRejectWithNoHandler is 0');
check(tq.promiseRejectEvents.kPromiseHandlerAddedAfterReject === 1, 'kPromiseHandlerAddedAfterReject is 1');
check(tq.promiseRejectEvents.kPromiseResolveAfterResolved === 2, 'kPromiseResolveAfterResolved is 2');
check(tq.promiseRejectEvents.kPromiseRejectAfterResolved === 3, 'kPromiseRejectAfterResolved is 3');

// --- async_context_frame binding exports ---
var acf = internalBinding('async_context_frame');
check(typeof acf.getContinuationPreservedEmbedderData === 'function',
  'getContinuationPreservedEmbedderData is a function');
check(typeof acf.setContinuationPreservedEmbedderData === 'function',
  'setContinuationPreservedEmbedderData is a function');

// --- process.nextTick availability ---
check(typeof process.nextTick === 'function', 'process.nextTick is a function');
check(typeof process._tickCallback === 'function', 'process._tickCallback is a function');

// --- Basic ordering ---
var log = [];

process.nextTick(function() {
  log.push('tick1');
});

process.nextTick(function() {
  log.push('tick2');
});

process.nextTick(function() {
  log.push('tick3');
});

log.push('sync');

// --- Nested nextTick ---
process.nextTick(function() {
  log.push('tick4');
  process.nextTick(function() {
    log.push('tick5-nested');

    // All ticks should have run in order by now.
    check(log[0] === 'sync', 'sync runs first');
    check(log[1] === 'tick1', 'tick1 runs second');
    check(log[2] === 'tick2', 'tick2 runs third');
    check(log[3] === 'tick3', 'tick3 runs fourth');
    check(log[4] === 'tick4', 'tick4 runs fifth');
    check(log[5] === 'tick5-nested', 'tick5-nested runs sixth');
  });
});

// --- nextTick with arguments ---
process.nextTick(function(a, b, c) {
  check(a === 42, 'nextTick arg 1');
  check(b === 'hello', 'nextTick arg 2');
  check(c === true, 'nextTick arg 3');
}, 42, 'hello', true);

// --- nextTick with no arguments besides callback ---
process.nextTick(function() {
  check(arguments.length === 0, 'nextTick with no extra args');
});

// --- nextTick callback receives correct this ---
process.nextTick(function() {
  // In strict mode, this should be undefined
  check(this === undefined, 'nextTick this is undefined in strict mode');
});

// --- enqueueMicrotask works ---
var microtaskRan = false;
tq.enqueueMicrotask(function() {
  microtaskRan = true;
});

// Check in a later tick (microtask runs before nextTick in the same drain cycle)
process.nextTick(function() {
  check(microtaskRan === true, 'enqueueMicrotask callback ran');
});

// --- Final check: print results ---
process.nextTick(function() {
  // This runs after all other ticks
  process.nextTick(function() {
    console.log('Passed:', passed);
    console.log('Failed:', failed);
    if (failed === 0) {
      console.log('PASS');
    }
  });
});
