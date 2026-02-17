// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

// Test the process_wrap native binding directly.

'use strict';

var assert = globalThis.assert;
if (!assert) {
  assert = function(cond, msg) {
    if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
  };
}

var assertEqual = function(a, b, msg) {
  if (a !== b) throw new Error('assertEqual failed: ' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + (msg ? ' ' + msg : ''));
};

// -- Test 1: binding exports --
var processWrap = internalBinding('process_wrap');
assert(typeof processWrap === 'object', 'process_wrap is an object');
assert(typeof processWrap.Process === 'function', 'Process is a constructor');

// -- Test 2: Process constructor --
var handle = new processWrap.Process();
assert(typeof handle === 'object', 'handle is an object');
assert(typeof handle.spawn === 'function', 'spawn is a function');
assert(typeof handle.kill === 'function', 'kill is a function');
assert(typeof handle.close === 'function', 'close is a function');
assert(typeof handle.ref === 'function', 'ref is a function');
assert(typeof handle.unref === 'function', 'unref is a function');

// -- Test 3: spawn a simple process (echo) --
var Pipe = internalBinding('pipe_wrap').Pipe;
var PipeConstants = internalBinding('pipe_wrap').constants;

var testsRemaining = 2; // test 3 + test 4

function maybeFinish() {
  testsRemaining--;
  if (testsRemaining === 0) {
    console.log('PASS');
  }
}

// Test 3: spawn echo and capture stdout
(function testSpawnEcho() {
  var proc = new processWrap.Process();
  var stdoutPipe = new Pipe(PipeConstants.SOCKET);

  proc.onexit = function(exitCode, signalCode) {
    assertEqual(exitCode, 0, 'echo should exit with code 0');
    assertEqual(signalCode, '', 'no signal for normal exit');
    proc.close();
    maybeFinish();
  };

  var err = proc.spawn({
    file: '/bin/echo',
    args: ['/bin/echo', 'hello-from-spawn'],
    cwd: undefined,
    envPairs: undefined,
    stdio: [
      { type: 'ignore' },
      { type: 'pipe', handle: stdoutPipe },
      { type: 'ignore' },
    ],
    detached: false,
    windowsHide: false,
    windowsVerbatimArguments: false,
  });

  assertEqual(err, 0, 'spawn should succeed');
  assert(typeof proc.pid === 'number', 'pid should be set');
  assert(proc.pid > 0, 'pid should be positive');

  // Read stdout from the pipe.
  var output = '';
  var streamWrap = internalBinding('stream_wrap');
  stdoutPipe.onread = function(arrayBuffer) {
    if (streamWrap.streamBaseState[streamWrap.kReadBytesOrError] > 0) {
      var buf = Buffer.from(arrayBuffer);
      output += buf.toString('utf8', 0, streamWrap.streamBaseState[streamWrap.kReadBytesOrError]);
    } else {
      // EOF or error
      stdoutPipe.readStop();
      stdoutPipe.close();
      assert(output.trim() === 'hello-from-spawn', 'stdout should contain echo output, got: ' + JSON.stringify(output.trim()));
    }
  };
  stdoutPipe.readStart();
})();

// Test 4: spawn and kill
(function testSpawnAndKill() {
  var proc = new processWrap.Process();

  proc.onexit = function(exitCode, signalCode) {
    // Process was killed by SIGTERM
    assertEqual(signalCode, 'SIGTERM', 'signal should be SIGTERM');
    proc.close();
    maybeFinish();
  };

  var err = proc.spawn({
    file: '/bin/sleep',
    args: ['/bin/sleep', '60'],
    cwd: undefined,
    envPairs: undefined,
    stdio: [
      { type: 'ignore' },
      { type: 'ignore' },
      { type: 'ignore' },
    ],
    detached: false,
    windowsHide: false,
    windowsVerbatimArguments: false,
  });

  assertEqual(err, 0, 'spawn sleep should succeed');
  assert(proc.pid > 0, 'sleep pid should be positive');

  // Kill the process.
  var killErr = proc.kill(15); // SIGTERM
  assertEqual(killErr, 0, 'kill should succeed');
})();
