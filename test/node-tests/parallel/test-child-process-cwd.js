// Adapted from Node.js test/parallel/test-child-process-cwd.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
const common = require('../common');
const tmpdir = require('../common/tmpdir');
tmpdir.refresh();

const assert = require('assert');
const { spawn } = require('child_process');

// Spawns 'pwd' with given options, then test:
// - whether the child pid is undefined or number
// - whether the exit code equals expectCode
// - optionally whether the trimmed stdout result matches expectData
function testCwd(options, expectPidType, expectCode, expectData, shouldCallExit) {
  if (expectCode === undefined) expectCode = 0;
  if (shouldCallExit === undefined) shouldCallExit = true;
  const child = spawn(...common.pwdCommand, options);

  assert.strictEqual(typeof child.pid, expectPidType);

  child.stdout.setEncoding('utf8');

  let data = '';
  child.stdout.on('data', function(chunk) {
    data += chunk;
  });

  child.on('exit', shouldCallExit ? common.mustCall(function(code) {
    assert.strictEqual(code, expectCode);
  }) : common.mustNotCall());

  child.on('close', common.mustCall(function() {
    if (expectData) {
      // In Windows, compare without considering case
      if (common.isWindows) {
        assert.strictEqual(data.trim().toLowerCase(), expectData.toLowerCase());
      } else {
        assert.strictEqual(data.trim(), expectData);
      }
    }
  }));

  return child;
}

// Assume does-not-exist doesn't exist, expect error with ENOENT.
{
  testCwd({ cwd: 'does-not-exist' }, 'undefined', -1, undefined, false)
    .on('error', common.mustCall(function(e) {
      assert.strictEqual(e.code, 'ENOENT');
    }));
}

// URL as cwd: invalid URLs should throw.
{
  // http URL: our shim throws about host (Node throws about scheme).
  assert.throws(function() {
    testCwd({
      cwd: new URL('http://example.com/'),
    }, 'number', 0, tmpdir.path);
  }, /File URL host must be "localhost" or empty on/);

  if (process.platform !== 'win32') {
    assert.throws(function() {
      testCwd({
        cwd: new URL('file://host/dev/null'),
      }, 'number', 0, tmpdir.path);
    }, /File URL host must be "localhost" or empty on/);
  }
}

// Assume these exist, and 'pwd' gives us the right directory back.
testCwd({ cwd: tmpdir.path }, 'number', 0, tmpdir.path);
const shouldExistDir = common.isWindows ? process.env.windir : '/dev';
testCwd({ cwd: shouldExistDir }, 'number', 0, shouldExistDir);
testCwd({ cwd: tmpdir.fileURL() }, 'number', 0, tmpdir.path);

// Spawn() shouldn't try to chdir() to invalid arg, so this should just work.
testCwd({ cwd: '' }, 'number');
testCwd({ cwd: undefined }, 'number');
testCwd({ cwd: null }, 'number');
