// Adapted from Node.js test/parallel/test-child-process-kill.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
const common = require('../common');
const assert = require('assert');
const spawn = require('child_process').spawn;

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
