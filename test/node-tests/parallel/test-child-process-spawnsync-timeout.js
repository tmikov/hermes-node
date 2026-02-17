// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

// Adapted from Node.js test/parallel/test-child-process-spawnsync-timeout.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
require('../common');
const assert = require('assert');
const spawnSync = require('child_process').spawnSync;
const { getSystemErrorName } = require('util');

const TIMER = 200;
const SLEEP = 5000;

const start = Date.now();
const ret = spawnSync('sleep', ['60'], { timeout: TIMER });

assert.strictEqual(ret.error.code, 'ETIMEDOUT');
assert.strictEqual(getSystemErrorName(ret.error.errno), 'ETIMEDOUT');

const end = Date.now() - start;
assert(end < SLEEP, 'should have timed out before SLEEP (' + end + 'ms)');
assert(ret.status > 128 || ret.signal,
       'process should have been killed by signal');
