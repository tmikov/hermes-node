// Adapted from Node.js test/parallel/test-child-process-spawnsync-input.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
const common = require('../common');

const assert = require('assert');

const spawnSync = require('child_process').spawnSync;

const msgOut = 'this is stdout';
const msgErr = 'this is stderr';

const msgOutBuf = Buffer.from(msgOut + '\n');
const msgErrBuf = Buffer.from(msgErr + '\n');

const args = [
  '-c',
  'echo "' + msgOut + '"; echo "' + msgErr + '" >&2',
];

let ret;

function checkSpawnSyncRet(ret) {
  assert.strictEqual(ret.status, 0);
  assert.strictEqual(ret.error, undefined);
}

function verifyBufOutput(ret) {
  checkSpawnSyncRet(ret);
  assert.deepStrictEqual(ret.stdout, msgOutBuf);
  assert.deepStrictEqual(ret.stderr, msgErrBuf);
}

// Test with 'sh -c' to echo stdout and stderr.
verifyBufOutput(spawnSync('sh', args));

// Test string input piped to cat.
const options = {
  input: 'hello world'
};

ret = spawnSync('cat', [], options);

checkSpawnSyncRet(ret);
assert.strictEqual(ret.stdout.toString('utf8'), options.input);
assert.strictEqual(ret.stderr.toString('utf8'), '');

// Test Buffer input piped to cat.
const bufOptions = {
  input: Buffer.from('hello world')
};

ret = spawnSync('cat', [], bufOptions);

checkSpawnSyncRet(ret);
assert.deepStrictEqual(ret.stdout, bufOptions.input);
assert.deepStrictEqual(ret.stderr, Buffer.from(''));

// Test encoding option.
ret = spawnSync('sh', args, { encoding: 'utf8' });

checkSpawnSyncRet(ret);
assert.strictEqual(ret.stdout, msgOut + '\n');
assert.strictEqual(ret.stderr, msgErr + '\n');

// Test invalid input type.
assert.throws(
  function() { spawnSync('cat', [], { input: 1234 }); },
  { code: 'ERR_INVALID_ARG_TYPE', name: 'TypeError' });
