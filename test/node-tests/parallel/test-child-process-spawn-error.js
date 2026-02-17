// Adapted from Node.js test/parallel/test-child-process-spawn-error.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
const common = require('../common');
const { getSystemErrorName } = require('util');
const spawn = require('child_process').spawn;
const assert = require('assert');
const fs = require('fs');

const enoentPath = 'foo123';
const spawnargs = ['bar'];
assert.strictEqual(fs.existsSync(enoentPath), false);

const enoentChild = spawn(enoentPath, spawnargs);

// Verify that stdio is setup if the error is not EMFILE or ENFILE.
assert.notStrictEqual(enoentChild.stdin, undefined);
assert.notStrictEqual(enoentChild.stdout, undefined);
assert.notStrictEqual(enoentChild.stderr, undefined);
assert(Array.isArray(enoentChild.stdio));
assert.strictEqual(enoentChild.stdio[0], enoentChild.stdin);
assert.strictEqual(enoentChild.stdio[1], enoentChild.stdout);
assert.strictEqual(enoentChild.stdio[2], enoentChild.stderr);

// Verify pid is not assigned.
assert.strictEqual(enoentChild.pid, undefined);

enoentChild.on('spawn', common.mustNotCall());

enoentChild.on('error', common.mustCall(function(err) {
  assert.strictEqual(err.code, 'ENOENT');
  assert.strictEqual(getSystemErrorName(err.errno), 'ENOENT');
  assert.strictEqual(err.syscall, 'spawn ' + enoentPath);
  assert.strictEqual(err.path, enoentPath);
  assert.deepStrictEqual(err.spawnargs, spawnargs);
}));
