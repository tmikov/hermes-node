// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// RUN: %hermes-node --node-version v18.12.0 %s override | %FileCheck %s
// CHECK: PASS
// Test: process.version reports the bundled Node version and stays in sync
// with process.versions.node, including under --node-version.

'use strict';

var assert = function (cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var override = process.argv[2] === 'override';

if (override) {
  assert(process.version === 'v18.12.0', 'version follows --node-version');
  assert(process.versions.node === '18.12.0', 'versions.node follows too');
} else {
  assert(process.version === 'v24.13.0', 'default version is the bundled one');
  assert(process.versions.node === '24.13.0', 'default versions.node');
}

// process.version is always 'v' + process.versions.node, as in Node.
assert(
  process.version === 'v' + process.versions.node,
  'version and versions.node agree',
);

// Packages gate on the major parsed out of process.version; yargs, for one,
// refuses to load below 12.
var major = Number(process.version.match(/v([^.]+)/)[1]);
assert(major >= 12, 'major version parses to something plausible');

console.log('PASS');
