// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test: fs.Utf8Stream is reachable and can write a file.

'use strict';

var assert = function (cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var fs = require('fs');
var os = require('os');
var path = require('path');

// Touching the property used to throw: the getter requires
// internal/streams/fast-utf8-stream, which was not embedded, and the module
// itself needed SharedArrayBuffer at load time.
assert(typeof fs.Utf8Stream === 'function', 'fs.Utf8Stream is a function');

var file = path.join(os.tmpdir(), 'hermes-node-utf8-stream-' + process.pid);

var stream = new fs.Utf8Stream({dest: file, sync: true});
stream.write('hello\n');
stream.write('world\n');
stream.flushSync();

assert(fs.readFileSync(file, 'utf8') === 'hello\nworld\n', 'contents written');

fs.unlinkSync(file);

console.log('PASS');
