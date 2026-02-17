// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
'use strict';

var assert = globalThis.assert;
if (!assert) {
  assert = require('assert');
}

var pending = 0;
function test(name, fn) {
  pending++;
  try {
    fn();
    pending--;
  } catch (e) {
    console.error('FAIL: ' + name + ': ' + e.message);
    console.error(e.stack);
    process.exit(1);
  }
}

// --- Test 1: spawnSync basic ---
test('spawnSync basic', function() {
  var child_process = require('child_process');
  var result = child_process.spawnSync('echo', ['sync-hello']);
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.signal, null);
  assert.strictEqual(result.stdout.toString().trim(), 'sync-hello');
  assert(result.pid > 0, 'pid should be positive');
});

// --- Test 2: spawnSync with stderr ---
test('spawnSync stderr', function() {
  var child_process = require('child_process');
  var result = child_process.spawnSync('sh', ['-c', 'echo err-output >&2']);
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.stderr.toString().trim(), 'err-output');
});

// --- Test 3: spawnSync with non-zero exit code ---
test('spawnSync exit code', function() {
  var child_process = require('child_process');
  var result = child_process.spawnSync('sh', ['-c', 'exit 42']);
  assert.strictEqual(result.status, 42);
  assert.strictEqual(result.signal, null);
});

// --- Test 4: spawnSync with input ---
test('spawnSync input', function() {
  var child_process = require('child_process');
  var result = child_process.spawnSync('cat', [], {
    input: 'hello-from-stdin'
  });
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.stdout.toString(), 'hello-from-stdin');
});

// --- Test 5: spawnSync with timeout ---
test('spawnSync timeout', function() {
  var child_process = require('child_process');
  var result = child_process.spawnSync('sleep', ['10'], {
    timeout: 100
  });
  // Should have been killed by timeout.
  assert(result.error, 'should have error');
  assert.strictEqual(result.error.code, 'ETIMEDOUT');
});

// --- Test 6: execSync ---
test('execSync', function() {
  var child_process = require('child_process');
  var output = child_process.execSync('echo exec-test').toString().trim();
  assert.strictEqual(output, 'exec-test');
});

// --- Test 7: execFileSync ---
test('execFileSync', function() {
  var child_process = require('child_process');
  var output = child_process.execFileSync('echo', ['execfile-test']).toString().trim();
  assert.strictEqual(output, 'execfile-test');
});

// --- Test 8: spawnSync with encoding ---
test('spawnSync encoding', function() {
  var child_process = require('child_process');
  var result = child_process.spawnSync('echo', ['encoded'], {
    encoding: 'utf8'
  });
  assert.strictEqual(result.status, 0);
  assert.strictEqual(typeof result.stdout, 'string');
  assert.strictEqual(result.stdout.trim(), 'encoded');
});

// --- Test 9: execSync with non-zero exit throws ---
test('execSync throws', function() {
  var child_process = require('child_process');
  var threw = false;
  try {
    child_process.execSync('sh -c "exit 1"');
  } catch (e) {
    threw = true;
    assert.strictEqual(e.status, 1);
  }
  assert(threw, 'execSync should throw on non-zero exit');
});

// --- Test 10: spawnSync with cwd ---
test('spawnSync cwd', function() {
  var child_process = require('child_process');
  var result = child_process.spawnSync('pwd', [], {
    cwd: '/tmp'
  });
  assert.strictEqual(result.status, 0);
  // The output should be /tmp (might be resolved to a real path).
  var out = result.stdout.toString().trim();
  assert(out === '/tmp' || out.endsWith('/tmp'), 'cwd should be /tmp, got: ' + out);
});

// --- Test 11: spawnSync with env ---
test('spawnSync env', function() {
  var child_process = require('child_process');
  var result = child_process.spawnSync('sh', ['-c', 'echo $MY_TEST_VAR'], {
    env: { MY_TEST_VAR: 'test-value', PATH: process.env.PATH }
  });
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.stdout.toString().trim(), 'test-value');
});

// --- Test 12: spawnSync with invalid command ---
test('spawnSync invalid command', function() {
  var child_process = require('child_process');
  var result = child_process.spawnSync('nonexistent-command-12345');
  assert(result.error, 'should have error for invalid command');
  assert.strictEqual(result.error.code, 'ENOENT');
});

// --- Test 13: spawnSync with maxBuffer ---
test('spawnSync maxBuffer', function() {
  var child_process = require('child_process');
  var threw = false;
  try {
    // Generate output larger than maxBuffer.
    child_process.execSync('yes | head -c 1000', { maxBuffer: 100 });
  } catch (e) {
    threw = true;
    // The error should be ENOBUFS or a message about maxBuffer.
    assert(e.code === 'ENOBUFS' || e.message.includes('maxBuffer') || e.status !== 0,
      'should be buffer overflow error');
  }
  assert(threw, 'should throw on maxBuffer exceeded');
});

assert.strictEqual(pending, 0, 'all tests should have run');
console.log('PASS');
