// Test: timers binding (setTimeout, setInterval, setImmediate, clearTimeout,
// clearInterval, clearImmediate, timer.unref)

'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var binding = internalBinding('timers');

// --- Binding exports ---
assert(typeof binding.setupTimers === 'function', 'setupTimers is function');
assert(typeof binding.getLibuvNow === 'function', 'getLibuvNow is function');
assert(typeof binding.scheduleTimer === 'function', 'scheduleTimer is function');
assert(typeof binding.toggleTimerRef === 'function', 'toggleTimerRef is function');
assert(typeof binding.toggleImmediateRef === 'function', 'toggleImmediateRef is function');

// immediateInfo: Uint32Array(3)
assert(binding.immediateInfo instanceof Uint32Array, 'immediateInfo is Uint32Array');
assert(binding.immediateInfo.length === 3, 'immediateInfo has 3 elements');

// timeoutInfo: Int32Array(1)
assert(binding.timeoutInfo instanceof Int32Array, 'timeoutInfo is Int32Array');
assert(binding.timeoutInfo.length === 1, 'timeoutInfo has 1 element');

// getLibuvNow returns a number >= 0
var now = binding.getLibuvNow();
assert(typeof now === 'number', 'getLibuvNow returns number');
assert(now >= 0, 'getLibuvNow returns non-negative');

// --- Timer globals exist ---
assert(typeof setTimeout === 'function', 'setTimeout is function');
assert(typeof clearTimeout === 'function', 'clearTimeout is function');
assert(typeof setInterval === 'function', 'setInterval is function');
assert(typeof clearInterval === 'function', 'clearInterval is function');
assert(typeof setImmediate === 'function', 'setImmediate is function');
assert(typeof clearImmediate === 'function', 'clearImmediate is function');

// --- Functional tests ---
var results = [];
var assertionErrors = [];

// 1. setImmediate fires before timers
setImmediate(function() {
  results.push('immediate');
});

// 2. setTimeout with 10ms
setTimeout(function() {
  results.push('timeout10');
}, 10);

// 3. setTimeout with 50ms
setTimeout(function() {
  results.push('timeout50');
}, 50);

// 4. setInterval (2 iterations then clear)
var intervalCount = 0;
var iv = setInterval(function() {
  intervalCount++;
  results.push('interval');
  if (intervalCount >= 2) {
    clearInterval(iv);
  }
}, 20);

// 5. clearTimeout cancels a timer
var cancelledFired = false;
var cancelled = setTimeout(function() {
  cancelledFired = true;
}, 15);
clearTimeout(cancelled);

// 6. clearImmediate cancels an immediate
var cancelledImmFired = false;
var cancelledImm = setImmediate(function() {
  cancelledImmFired = true;
});
clearImmediate(cancelledImm);

// 7. setTimeout with arguments
setTimeout(function(a, b) {
  if (a !== 'hello' || b !== 42) {
    assertionErrors.push('setTimeout args: expected hello,42 got ' + a + ',' + b);
  }
  results.push('withArgs');
}, 5, 'hello', 42);

// 8. process.nextTick fires before timers
process.nextTick(function() {
  results.push('nextTick');
});

// 9. Nested setTimeout
setTimeout(function() {
  setTimeout(function() {
    results.push('nested');
  }, 5);
}, 10);

// 10. Timer.unref() - the unrefed timer should not keep the event loop alive
var unrefTimer = setTimeout(function() {
  // This should NOT fire because the process should exit before 10s
  assertionErrors.push('unrefed timer should not fire');
}, 10000);
unrefTimer.unref();

// Final check
setTimeout(function() {
  // Verify results
  try {
    // Check immediate fired
    assert(results.indexOf('immediate') >= 0, 'immediate fired');

    // Check immediate before timers
    assert(
      results.indexOf('immediate') < results.indexOf('timeout10'),
      'immediate fires before timeout10'
    );

    // Check nextTick before timers
    assert(
      results.indexOf('nextTick') < results.indexOf('timeout10'),
      'nextTick fires before timeout10'
    );

    // Check timeouts fired
    assert(results.indexOf('timeout10') >= 0, 'timeout10 fired');
    assert(results.indexOf('timeout50') >= 0, 'timeout50 fired');

    // Check timeout ordering
    assert(
      results.indexOf('timeout10') < results.indexOf('timeout50'),
      'timeout10 fires before timeout50'
    );

    // Check interval fired twice
    var intervalFires = results.filter(function(r) { return r === 'interval'; });
    assert(intervalFires.length >= 2, 'interval fired at least 2 times');

    // Check clearTimeout worked
    assert(!cancelledFired, 'clearTimeout prevented firing');

    // Check clearImmediate worked
    assert(!cancelledImmFired, 'clearImmediate prevented firing');

    // Check setTimeout with args worked
    assert(results.indexOf('withArgs') >= 0, 'setTimeout with args worked');

    // Check nested setTimeout worked
    assert(results.indexOf('nested') >= 0, 'nested setTimeout worked');

    // Check no assertion errors from callbacks
    if (assertionErrors.length > 0) {
      throw new Error('Callback errors: ' + assertionErrors.join('; '));
    }

    console.log('PASS');
  } catch (e) {
    console.error('FAIL:', e.message);
    console.error('Results:', JSON.stringify(results));
    process.exit(1);
  }
}, 200);
