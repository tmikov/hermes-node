// Adapted from Node.js test/parallel/test-child-process-spawn-args.mjs
// Converted from ESM to CJS for hermes-node compatibility.
// Tests that undefined, null, and [] can be used as placeholder for args.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
const common = require('../common');
const tmpdir = require('../common/tmpdir');
const assert = require('assert');
const { spawn } = require('child_process');

tmpdir.refresh();

const command = 'pwd';
const options = { cwd: tmpdir.path };

const testCases = [
  undefined,
  null,
  [],
];

const expectedResult = tmpdir.path.trim().toLowerCase();

let completed = 0;

testCases.forEach(function(testCase) {
  const subprocess = spawn(command, testCase, options);

  let accumulatedData = '';

  subprocess.stdout.setEncoding('utf8');
  subprocess.stdout.on('data', common.mustCallAtLeast(function(data) {
    accumulatedData += data;
  }));

  subprocess.stdout.on('end', common.mustCall(function() {
    const result = accumulatedData.trim().toLowerCase();
    assert.strictEqual(result, expectedResult,
      'args=' + JSON.stringify(testCase) + ': expected ' + expectedResult + ', got ' + result);
    completed++;
    if (completed === testCases.length) {
      // All test cases passed.
    }
  }));
});
