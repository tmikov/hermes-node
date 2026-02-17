// Adapted from Node.js test/parallel/test-child-process-kill.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
const common = require('../common');
const assert = require('assert');
const spawn = require('child_process').spawn;

// Child mode: wait for stdin data, then exit.
if (process.argv[2] === 'child-wait') {
  var interval = setInterval(function() {}, 1000);
  process.stdin.on('data', function() { clearInterval(interval); });
  process.stdout.write('x');
  return;
}

// Test 1: kill cat process with default signal (SIGTERM).
const cat = spawn('cat');

cat.stdout.on('end', common.mustCall());
cat.stderr.on('data', common.mustNotCall());
cat.stderr.on('end', common.mustCall());

cat.on('exit', common.mustCall(function(code, signal) {
  assert.strictEqual(code, null);
  assert.strictEqual(signal, 'SIGTERM');
  assert.strictEqual(cat.signalCode, 'SIGTERM');
}));

assert.strictEqual(cat.signalCode, null);
assert.strictEqual(cat.killed, false);
cat.kill();
assert.strictEqual(cat.killed, true);

// Test 2: Windows signal remapping.
if (common.isWindows) {
  var signals = ['SIGTERM', 'SIGKILL', 'SIGQUIT', 'SIGINT'];
  for (var i = 0; i < signals.length; i++) {
    var sendSignal = signals[i];
    var proc = spawn('cmd');
    proc.on('exit', function(code, signal) {
      assert.strictEqual(code, null);
      assert.strictEqual(signal, sendSignal);
    });
    proc.kill(sendSignal);
  }

  // Non-standard signal should be remapped to SIGKILL.
  var proc2 = spawn('cmd');
  proc2.on('exit', function(code, signal) {
    assert.strictEqual(code, null);
    assert.strictEqual(signal, 'SIGKILL');
  });
  proc2.kill('SIGHUP');
}

// Test 3: kill(0) is a no-op that checks process existence.
// Spawns self in child-wait mode: child writes 'x' to stdout, waits for stdin.
// Parent receives 'x', calls kill(0) (no-op), then sends data to stdin.
// Child exits normally.
const checkProcess = spawn(process.execPath, common.spawnArgs(__filename, 'child-wait'));

checkProcess.on('exit', common.mustCall(function(code, signal) {
  assert.strictEqual(code, 0);
  assert.strictEqual(signal, null);
}));

checkProcess.stdout.on('data', common.mustCall(function(chunk) {
  assert.strictEqual(chunk.toString(), 'x');
  checkProcess.kill(0);
  checkProcess.stdin.write('x');
  checkProcess.stdin.end();
}));
