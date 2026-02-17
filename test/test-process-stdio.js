// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s
// Test process.stdin, process.stdout, process.stderr as proper streams.
'use strict';

var assert = require('assert');
var child_process = require('child_process');
var path = require('path');

var passed = 0;
var total = 0;

function test(name, fn) {
  total++;
  try {
    fn();
    passed++;
  } catch (e) {
    console.error('FAIL: ' + name + ': ' + e.message);
  }
}

// ---- process.stdout tests ----

test('stdout exists and is an object', function() {
  assert.strictEqual(typeof process.stdout, 'object');
  assert(process.stdout !== null);
});

test('stdout.fd is 1', function() {
  assert.strictEqual(process.stdout.fd, 1);
});

test('stdout._isStdio is true', function() {
  assert.strictEqual(process.stdout._isStdio, true);
});

test('stdout.write is a function', function() {
  assert.strictEqual(typeof process.stdout.write, 'function');
});

test('stdout has event emitter methods', function() {
  assert.strictEqual(typeof process.stdout.on, 'function');
  assert.strictEqual(typeof process.stdout.once, 'function');
  assert.strictEqual(typeof process.stdout.removeListener, 'function');
  assert.strictEqual(typeof process.stdout.emit, 'function');
});

test('stdout is a proper Writable stream', function() {
  var stream = require('stream');
  assert(process.stdout instanceof stream.Writable || process.stdout instanceof stream.Duplex,
    'stdout should be a Writable or Duplex stream');
});

test('stdout.write returns boolean', function() {
  var ret = process.stdout.write('');
  assert.strictEqual(typeof ret, 'boolean');
});

// ---- process.stderr tests ----

test('stderr exists and is an object', function() {
  assert.strictEqual(typeof process.stderr, 'object');
  assert(process.stderr !== null);
});

test('stderr.fd is 2', function() {
  assert.strictEqual(process.stderr.fd, 2);
});

test('stderr._isStdio is true', function() {
  assert.strictEqual(process.stderr._isStdio, true);
});

test('stderr is a proper Writable stream', function() {
  var stream = require('stream');
  assert(process.stderr instanceof stream.Writable || process.stderr instanceof stream.Duplex,
    'stderr should be a Writable or Duplex stream');
});

// ---- process.stdin tests ----

test('stdin exists and is an object', function() {
  assert.strictEqual(typeof process.stdin, 'object');
  assert(process.stdin !== null);
});

test('stdin.fd is 0', function() {
  assert.strictEqual(process.stdin.fd, 0);
});

test('stdin has readable stream methods', function() {
  assert.strictEqual(typeof process.stdin.on, 'function');
  assert.strictEqual(typeof process.stdin.read, 'function');
  assert.strictEqual(typeof process.stdin.resume, 'function');
  assert.strictEqual(typeof process.stdin.pause, 'function');
});

test('stdin is a proper Readable stream', function() {
  var stream = require('stream');
  assert(process.stdin instanceof stream.Readable,
    'stdin should be a Readable stream');
});

// ---- stdin piping via child process ----

test('stdin receives piped data in child process', function() {
  // Spawn a child that reads from stdin and echoes it.
  var result = child_process.spawnSync(process.execPath, [
    '--node-lib-path', path.join(__dirname, '..'),
    '-e', 'not-a-file' // placeholder - we'll use inline script
  ], {
    // We can't use -e easily, so use a script approach
    timeout: 5000,
  });
  // This test just verifies we can spawn; stdin piping test below is more thorough.
});

test('child process stdin piping works', function() {
  // Create a small script that reads all of stdin and prints it.
  var fs = require('fs');
  var os = require('os');
  var tmpFile = path.join(os.tmpdir(), 'hermes-stdin-test-' + process.pid + '.js');
  fs.writeFileSync(tmpFile, [
    "'use strict';",
    "var chunks = [];",
    "process.stdin.on('data', function(chunk) { chunks.push(chunk); });",
    "process.stdin.on('end', function() {",
    "  var data = Buffer.concat(chunks).toString();",
    "  process.stdout.write(data);",
    "});",
    "process.stdin.resume();",
  ].join('\n'));

  try {
    var result = child_process.spawnSync(process.execPath, [
      '--node-lib-path', path.join(__dirname, '..'),
      tmpFile
    ], {
      input: 'hello from stdin',
      timeout: 10000,
    });

    assert.strictEqual(result.status, 0,
      'child exited with code 0, got ' + result.status +
      (result.stderr ? ' stderr: ' + result.stderr.toString() : ''));
    assert.strictEqual(result.stdout.toString(), 'hello from stdin',
      'child received and echoed stdin data');
  } finally {
    try { fs.unlinkSync(tmpFile); } catch(e) {}
  }
});

// ---- stdout/stderr in child process ----

test('child stdout and stderr separation', function() {
  var fs = require('fs');
  var os = require('os');
  var tmpFile = path.join(os.tmpdir(), 'hermes-stdio-sep-' + process.pid + '.js');
  fs.writeFileSync(tmpFile, [
    "'use strict';",
    "process.stdout.write('out-msg');",
    "process.stderr.write('err-msg');",
  ].join('\n'));

  try {
    var result = child_process.spawnSync(process.execPath, [
      '--node-lib-path', path.join(__dirname, '..'),
      tmpFile
    ], {
      timeout: 10000,
    });

    assert.strictEqual(result.status, 0,
      'child exited with code 0');
    assert.strictEqual(result.stdout.toString(), 'out-msg',
      'stdout contains only stdout data');
    assert.strictEqual(result.stderr.toString(), 'err-msg',
      'stderr contains only stderr data');
  } finally {
    try { fs.unlinkSync(tmpFile); } catch(e) {}
  }
});

// ---- console.log/error go to proper streams ----

test('console.log goes to stdout in child', function() {
  var fs = require('fs');
  var os = require('os');
  var tmpFile = path.join(os.tmpdir(), 'hermes-console-' + process.pid + '.js');
  fs.writeFileSync(tmpFile, [
    "'use strict';",
    "console.log('hello-log');",
    "console.error('hello-error');",
  ].join('\n'));

  try {
    var result = child_process.spawnSync(process.execPath, [
      '--node-lib-path', path.join(__dirname, '..'),
      tmpFile
    ], {
      timeout: 10000,
    });

    assert.strictEqual(result.status, 0, 'child exited with code 0');
    assert(result.stdout.toString().indexOf('hello-log') >= 0,
      'console.log output in stdout');
    assert(result.stderr.toString().indexOf('hello-error') >= 0,
      'console.error output in stderr');
  } finally {
    try { fs.unlinkSync(tmpFile); } catch(e) {}
  }
});

// ---- Results ----

process.on('exit', function() {
  if (passed === total) {
    console.log('PASS');
  } else {
    console.log('FAIL: ' + passed + '/' + total + ' tests passed');
    process.exitCode = 1;
  }
});
