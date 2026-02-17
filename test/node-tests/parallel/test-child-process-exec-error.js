// Adapted from Node.js test/parallel/test-child-process-exec-error.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
const common = require('../common');
const assert = require('assert');
const child_process = require('child_process');

function test(fn, code, expectPidType) {
  if (expectPidType === undefined) expectPidType = 'number';
  const child = fn('does-not-exist', common.mustCall(function(err) {
    assert.strictEqual(err.code, code);
    assert(err.cmd.includes('does-not-exist'));
  }));

  assert.strictEqual(typeof child.pid, expectPidType);
}

// With `shell: true` (exec), expect pid (of the shell).
// Exit code 127 from /bin/sh for command not found.
test(child_process.exec, 127, 'number');

// With `shell: false` (execFile), expect no pid.
test(child_process.execFile, 'ENOENT', 'undefined');
