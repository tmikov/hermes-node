// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s

// End-to-end integration test for --inspect and --inspect-brk.
// Uses child_process.spawnSync to launch hermes-node with inspector flags
// and verify stderr messages, stdout output, and clean exit.
// Port 0 (OS-assigned) avoids conflicts in parallel test runs.

'use strict';

var assert = require('assert');
var child_process = require('child_process');

var hermesNode = process.execPath;

function run(args, options) {
  return child_process.spawnSync(hermesNode, args, Object.assign(
    { encoding: 'utf8', timeout: 10000 },
    options
  ));
}

// --- Test 1: --inspect=0 starts the inspector and exits cleanly ---

var r1 = run(['--inspect=0', '-e', "console.log('PASS')"]);
assert.strictEqual(r1.status, 0,
  'Test 1: expected exit code 0, got ' + r1.status +
  '\nstderr: ' + (r1.stderr || '').substring(0, 500));
assert(r1.stderr.indexOf('Debugger listening on ws://') >= 0,
  'Test 1: stderr should contain "Debugger listening on ws://"' +
  '\nstderr: ' + r1.stderr);
assert(r1.stderr.indexOf('For help, see:') >= 0,
  'Test 1: stderr should contain help URL');
assert(r1.stderr.indexOf('Open DevTools:') >= 0,
  'Test 1: stderr should contain DevTools URL');
assert.strictEqual(r1.stdout.trim(), 'PASS',
  'Test 1: stdout should contain PASS');

// --- Test 2: --inspect=0 with an empty script exits cleanly ---

var r2 = run(['--inspect=0', '-e', '']);
assert.strictEqual(r2.status, 0,
  'Test 2: empty script with --inspect should exit 0, got ' + r2.status +
  '\nstderr: ' + (r2.stderr || '').substring(0, 500));
assert(r2.stderr.indexOf('Debugger listening') >= 0,
  'Test 2: stderr should contain "Debugger listening"');

// --- Test 3: --inspect-brk=0 starts and shows debugger message ---
// With --inspect-brk the runtime pauses before user code, so the script
// cannot complete without a debugger client sending Debugger.resume.
// We use a short timeout and verify the process was killed (timeout),
// and that the debugger message appeared in stderr.

var r3 = run(['--inspect-brk=0', '-e', "console.log('SHOULD_NOT_PRINT')"],
  { timeout: 3000 });
// spawnSync with timeout: status is null and signal is SIGTERM on timeout.
assert(r3.status === null || r3.status !== 0,
  'Test 3: --inspect-brk should not exit 0 within timeout (paused)');
assert(r3.stderr.indexOf('Debugger listening on ws://') >= 0,
  'Test 3: stderr should contain "Debugger listening on ws://"' +
  '\nstderr: ' + r3.stderr);
// The script should NOT have printed because it's paused at first line.
assert(r3.stdout.indexOf('SHOULD_NOT_PRINT') < 0,
  'Test 3: stdout should NOT contain script output (paused at first line)');

// --- Test 4: --inspect=HOST:PORT parses correctly ---

var r4 = run(['--inspect=127.0.0.1:0', '-e', "console.log('PASS4')"]);
assert.strictEqual(r4.status, 0,
  'Test 4: expected exit code 0, got ' + r4.status +
  '\nstderr: ' + (r4.stderr || '').substring(0, 500));
assert(r4.stderr.indexOf('Debugger listening on ws://127.0.0.1:') >= 0,
  'Test 4: stderr should contain host:port in debugger URL');
assert.strictEqual(r4.stdout.trim(), 'PASS4',
  'Test 4: stdout should contain PASS4');

// --- Test 5: stderr contains a valid ws:// URL with UUID ---

var match = r1.stderr.match(/Debugger listening on (ws:\/\/[^\s]+)/);
assert(match, 'Test 5: should find ws:// URL in stderr');
var wsUrl = match[1];
// URL should contain host:port/uuid pattern.
assert(/ws:\/\/127\.0\.0\.1:\d+\/[0-9a-f-]+/.test(wsUrl),
  'Test 5: ws URL should match ws://127.0.0.1:PORT/UUID pattern, got: ' + wsUrl);

console.log('PASS');
