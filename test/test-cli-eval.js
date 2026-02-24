// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s

'use strict';

var assert = require('assert');
var child_process = require('child_process');

var hermesNode = process.execPath;

function run(args) {
  return child_process.spawnSync(hermesNode, args, { encoding: 'utf8' });
}

function assertSuccess(result, label) {
  assert.strictEqual(result.status, 0, label + ': expected exit code 0, got ' + result.status);
}

function assertFailure(result, label) {
  assert.notStrictEqual(result.status, 0, label + ': expected non-zero exit code');
}

var r1 = run(['-e', "console.log('short-e')"]);
assertSuccess(r1, 'short -e');
assert.strictEqual(r1.stdout.trim(), 'short-e', 'short -e: stdout mismatch');

var r2 = run(['--eval', "console.log('long-eval')"]);
assertSuccess(r2, '--eval');
assert.strictEqual(r2.stdout.trim(), 'long-eval', '--eval: stdout mismatch');

var r3 = run(["--eval=console.log('eq-eval')"]);
assertSuccess(r3, '--eval=');
assert.strictEqual(r3.stdout.trim(), 'eq-eval', '--eval=: stdout mismatch');

var r4 = run(['-e', "console.log('first')", '-e', "console.log('second')"]);
assertSuccess(r4, 'multiple -e');
assert.strictEqual(r4.stdout.trim(), 'second', 'multiple -e: expected last eval to win');

var r5 = run(['-e', 'console.log(JSON.stringify(process.argv))', 'arg1', '--flag']);
assertSuccess(r5, 'eval argv');
var argvOut = JSON.parse(r5.stdout.trim());
assert.strictEqual(argvOut[0], hermesNode, 'eval argv[0] should be hermes-node path');
assert.strictEqual(argvOut[1], 'arg1', 'eval argv[1] should be first CLI arg after eval');
assert.strictEqual(argvOut[2], '--flag', 'eval argv[2] should preserve option-like args');

var r6 = run(['-e']);
assertFailure(r6, 'missing -e argument');
assert(r6.stderr.includes('requires a value'), 'missing -e argument: expected error text');

var r7 = run(['--eval']);
assertFailure(r7, 'missing --eval argument');
assert(r7.stderr.includes('requires a value'), 'missing --eval argument: expected error text');

console.log('PASS');
