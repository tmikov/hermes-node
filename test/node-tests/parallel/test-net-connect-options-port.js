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

// Ported from Node.js test/parallel/test-net-connect-options-port.js
// Focuses on port validation (sync error paths) and a single valid connection.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
var common = require('../common');
var assert = require('assert');
var net = require('net');

// Test wrong type of ports
{
  var portTypeError = {
    code: 'ERR_INVALID_ARG_TYPE',
    name: 'TypeError'
  };

  syncFailToConnect(true, portTypeError);
  syncFailToConnect(false, portTypeError);
  syncFailToConnect([], portTypeError);
  syncFailToConnect({}, portTypeError);
  syncFailToConnect(null, portTypeError);
}

// Test out of range ports
{
  var portRangeError = {
    code: 'ERR_SOCKET_BAD_PORT',
    name: 'RangeError'
  };

  syncFailToConnect('', portRangeError);
  syncFailToConnect(' ', portRangeError);
  syncFailToConnect(NaN, portRangeError);
  syncFailToConnect(Infinity, portRangeError);
  syncFailToConnect(-1, portRangeError);
  syncFailToConnect(65536, portRangeError);
}

// Test valid port connection
{
  var server = net.createServer(common.mustCall(function(socket) {
    socket.end('ok');
    server.close();
  }));

  server.listen(0, '127.0.0.1', common.mustCall(function() {
    var port = server.address().port;
    // Connect with integer port via options object
    var client = net.connect({ port: port, family: 4 });
    client.on('data', function() {});
    client.on('end', common.mustCall(function() {
      client.end();
    }));
    client.on('error', function() {});
  }));
}

function syncFailToConnect(port, assertErr) {
  assert.throws(function() {
    net.connect({ port: port, family: 4 });
  }, assertErr, 'connect({port: ' + port + '})');

  assert.throws(function() {
    net.connect({ port: port, host: 'localhost', family: 4 });
  }, assertErr, "connect({port: " + port + ", host: 'localhost'})");
}
