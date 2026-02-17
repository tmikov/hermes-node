// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s

// Comprehensive verification of the child_process module.
// Uses exit-code-based testing (not FileCheck pipe) because async child
// process cleanup can race with SIGPIPE when stdout is piped to FileCheck.

'use strict';

var assert = globalThis.assert;
if (!assert) {
  assert = require('assert');
}

var child_process = require('child_process');
var path = require('path');

var pending = 0;
var asyncTests = 0;
var asyncDone = 0;

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

function asyncTest(name, fn) {
  asyncTests++;
  fn(function done() {
    asyncDone++;
    checkAllDone();
  });
}

function checkAllDone() {
  if (asyncDone === asyncTests) {
    console.log('PASS');
  }
}

// --- Test 1: spawn with stdio pipe (capture stdout) ---
asyncTest('spawn stdout capture', function(done) {
  var child = child_process.spawn('echo', ['spawn-hello']);
  var output = '';
  child.stdout.on('data', function(data) {
    output += data;
  });
  child.on('close', function(code) {
    assert.strictEqual(code, 0, 'echo should exit with code 0');
    assert.strictEqual(output.trim(), 'spawn-hello', 'stdout should contain echo output');
    done();
  });
});

// --- Test 2: exec callback API ---
asyncTest('exec callback', function(done) {
  child_process.exec('echo exec-test', function(err, stdout, stderr) {
    assert.ifError(err);
    assert.strictEqual(stdout.trim(), 'exec-test');
    assert.strictEqual(typeof stderr, 'string');
    done();
  });
});

// --- Test 3: execSync ---
test('execSync', function() {
  var output = child_process.execSync('echo execsync-test').toString().trim();
  assert.strictEqual(output, 'execsync-test');
});

// --- Test 4: spawnSync ---
test('spawnSync', function() {
  var result = child_process.spawnSync('echo', ['spawnsync-test']);
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.signal, null);
  assert.strictEqual(result.stdout.toString().trim(), 'spawnsync-test');
  assert(result.pid > 0, 'pid should be positive');
});

// --- Test 5: execFile + execFileSync ---
asyncTest('execFile callback', function(done) {
  child_process.execFile('echo', ['execfile-test'], function(err, stdout) {
    assert.ifError(err);
    assert.strictEqual(stdout.trim(), 'execfile-test');
    done();
  });
});

test('execFileSync', function() {
  var output = child_process.execFileSync('echo', ['execfilesync-test']).toString().trim();
  assert.strictEqual(output, 'execfilesync-test');
});

// --- Test 6: Child environment variables ---
asyncTest('spawn with env', function(done) {
  var child = child_process.spawn('sh', ['-c', 'echo $MY_TEST_VAR'], {
    env: { MY_TEST_VAR: 'hello-env', PATH: process.env.PATH }
  });
  var output = '';
  child.stdout.on('data', function(data) { output += data; });
  child.on('close', function(code) {
    assert.strictEqual(code, 0);
    assert.strictEqual(output.trim(), 'hello-env');
    done();
  });
});

test('spawnSync with env', function() {
  var result = child_process.spawnSync('sh', ['-c', 'echo $MY_SYNC_VAR'], {
    env: { MY_SYNC_VAR: 'sync-env-val', PATH: process.env.PATH }
  });
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.stdout.toString().trim(), 'sync-env-val');
});

// --- Test 7: Child working directory ---
asyncTest('spawn with cwd', function(done) {
  var child = child_process.spawn('pwd', [], { cwd: '/tmp' });
  var output = '';
  child.stdout.on('data', function(data) { output += data; });
  child.on('close', function(code) {
    assert.strictEqual(code, 0);
    var out = output.trim();
    // /tmp may resolve to a different real path on some systems.
    assert(out === '/tmp' || out.endsWith('/tmp'), 'cwd should be /tmp, got: ' + out);
    done();
  });
});

test('spawnSync with cwd', function() {
  var result = child_process.spawnSync('pwd', [], { cwd: '/tmp' });
  assert.strictEqual(result.status, 0);
  var out = result.stdout.toString().trim();
  assert(out === '/tmp' || out.endsWith('/tmp'), 'cwd should be /tmp, got: ' + out);
});

// --- Test 8: Signal handling (kill child) ---
asyncTest('spawn and kill', function(done) {
  var child = child_process.spawn('sleep', ['60']);
  child.on('exit', function(code, signal) {
    assert.strictEqual(signal, 'SIGTERM');
    done();
  });
  // Kill after a short delay to ensure child is running.
  setTimeout(function() {
    child.kill('SIGTERM');
  }, 50);
});

// --- Test 9: Exit code propagation ---
asyncTest('exit code propagation', function(done) {
  var child = child_process.spawn('sh', ['-c', 'exit 42']);
  child.on('exit', function(code, signal) {
    assert.strictEqual(code, 42);
    assert.strictEqual(signal, null);
    done();
  });
});

test('spawnSync exit code', function() {
  var result = child_process.spawnSync('sh', ['-c', 'exit 7']);
  assert.strictEqual(result.status, 7);
  assert.strictEqual(result.signal, null);
});

// --- Test 10: stderr capture ---
asyncTest('spawn stderr capture', function(done) {
  var child = child_process.spawn('sh', ['-c', 'echo error-output >&2']);
  var stderr = '';
  child.stderr.on('data', function(data) { stderr += data; });
  child.on('close', function(code) {
    assert.strictEqual(code, 0);
    assert.strictEqual(stderr.trim(), 'error-output');
    done();
  });
});

test('spawnSync stderr capture', function() {
  var result = child_process.spawnSync('sh', ['-c', 'echo sync-error >&2']);
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.stderr.toString().trim(), 'sync-error');
});

// --- Test 11: exec with timeout ---
asyncTest('exec with timeout', function(done) {
  child_process.exec('sleep 60', { timeout: 100 }, function(err, stdout, stderr) {
    assert(err, 'should have error');
    assert(err.killed, 'should be killed');
    done();
  });
});

// --- Test 12: spawnSync with input ---
test('spawnSync with input', function() {
  var result = child_process.spawnSync('cat', [], {
    input: 'hello-from-input'
  });
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.stdout.toString(), 'hello-from-input');
});

// --- Test 13: spawnSync with encoding ---
test('spawnSync with encoding', function() {
  var result = child_process.spawnSync('echo', ['encoded-msg'], {
    encoding: 'utf8'
  });
  assert.strictEqual(result.status, 0);
  assert.strictEqual(typeof result.stdout, 'string');
  assert.strictEqual(result.stdout.trim(), 'encoded-msg');
});

// --- Test 14: spawn with invalid command ---
asyncTest('spawn invalid command', function(done) {
  var child = child_process.spawn('nonexistent-command-12345');
  child.on('error', function(err) {
    assert.strictEqual(err.code, 'ENOENT');
    done();
  });
});

// --- Test 15: spawnSync with invalid command ---
test('spawnSync invalid command', function() {
  var result = child_process.spawnSync('nonexistent-command-12345');
  assert(result.error, 'should have error');
  assert.strictEqual(result.error.code, 'ENOENT');
});

// --- Verify all synchronous tests passed ---
assert.strictEqual(pending, 0, 'all sync tests should have run');

// Async tests will print PASS when all done.
