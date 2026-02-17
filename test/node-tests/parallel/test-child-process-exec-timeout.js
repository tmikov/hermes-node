// Adapted from Node.js test/parallel/test-child-process-exec-timeout-expire.js
// and test-child-process-exec-timeout-kill.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
const common = require('../common');
const assert = require('assert');
const cp = require('child_process');

if (process.argv[2] === 'child') {
  // Run for a long time (will be killed by timeout).
  setTimeout(function() {
    console.log('child stdout');
    console.error('child stderr');
  }, 20000);
  return;
}

// Test 1: exec() with a timeout that expires (default SIGTERM).
const cmd1 = process.execPath + ' ' + __filename + ' child';
cp.exec(cmd1, {
  timeout: 1,
}, common.mustCall(function(err, stdout, stderr) {
  assert.strictEqual(err.killed, true);
  assert.strictEqual(err.code, null);
  // On macOS, SIGTERM can become SIGKILL for processes still starting up.
  if (common.isMacOS)
    assert(err.signal === 'SIGTERM' || err.signal === 'SIGKILL');
  else
    assert.strictEqual(err.signal, 'SIGTERM');
  assert.strictEqual(stdout.trim(), '');
  assert.strictEqual(stderr.trim(), '');
}));

// Test 2: exec() with a timeout and killSignal: SIGKILL.
const cmd2 = process.execPath + ' ' + __filename + ' child';
cp.exec(cmd2, {
  timeout: 1,
  killSignal: 'SIGKILL',
}, common.mustCall(function(err, stdout, stderr) {
  assert.strictEqual(err.killed, true);
  assert.strictEqual(err.code, null);
  assert.strictEqual(err.signal, 'SIGKILL');
  assert.strictEqual(stdout.trim(), '');
  assert.strictEqual(stderr.trim(), '');
}));
