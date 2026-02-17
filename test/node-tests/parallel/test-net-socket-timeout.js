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

// Ported from Node.js test/parallel/test-net-socket-timeout.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
var common = require('../common');
var net = require('net');
var assert = require('assert');

// Verify that invalid delays throw
var s = new net.Socket();
var nonNumericDelays = [
  '100', true, false, undefined, null, '', {}, function() {}, [],
];
var badRangeDelays = [-0.001, -1, -Infinity, Infinity, NaN];
var validDelays = [0, 0.001, 1, 1e6];
var invalidCallbacks = [
  1, '100', true, false, null, {}, [], Symbol('test'),
];


for (var i = 0; i < nonNumericDelays.length; i++) {
  assert.throws(function() {
    s.setTimeout(nonNumericDelays[i], function() {});
  }, { code: 'ERR_INVALID_ARG_TYPE' }, String(nonNumericDelays[i]));
}

for (var i = 0; i < badRangeDelays.length; i++) {
  assert.throws(function() {
    s.setTimeout(badRangeDelays[i], function() {});
  }, { code: 'ERR_OUT_OF_RANGE' }, String(badRangeDelays[i]));
}

for (var i = 0; i < validDelays.length; i++) {
  s.setTimeout(validDelays[i], function() {});
}

for (var i = 0; i < invalidCallbacks.length; i++) {
  [0, 1].forEach(function(msec) {
    assert.throws(
      function() { s.setTimeout(msec, invalidCallbacks[i]); },
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      }
    );
  });
}

var server = net.Server();
server.listen(0, common.mustCall(function() {
  var socket = net.createConnection(server.address().port);
  assert.strictEqual(
    socket.setTimeout(1, common.mustCall(function() {
      socket.destroy();
      assert.strictEqual(socket.setTimeout(1, common.mustNotCall()), socket);
      server.close();
    })),
    socket
  );
}));
