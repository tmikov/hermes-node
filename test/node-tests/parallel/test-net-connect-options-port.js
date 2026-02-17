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
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
var common = require('../common');
var assert = require('assert');
var dns = require('dns');
var net = require('net');

// Test wrong type of ports
{
  var portTypeError = {
    code: 'ERR_INVALID_ARG_TYPE',
    name: 'TypeError'
  };

  syncFailToConnect(true, portTypeError);
  syncFailToConnect(false, portTypeError);
  syncFailToConnect([], portTypeError, true);
  syncFailToConnect({}, portTypeError, true);
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
  syncFailToConnect('0x', portRangeError, true);
  syncFailToConnect('-0x1', portRangeError, true);
  syncFailToConnect(NaN, portRangeError);
  syncFailToConnect(Infinity, portRangeError);
  syncFailToConnect(-1, portRangeError);
  syncFailToConnect(65536, portRangeError);
}

// Test invalid hints
{
  var hints = (dns.ADDRCONFIG | dns.V4MAPPED | dns.ALL) + 42;
  var hintOptBlocks = doConnect([{ port: 42, hints: hints }],
                                function() { return common.mustNotCall(); });
  for (var i = 0; i < hintOptBlocks.length; i++) {
    assert.throws(hintOptBlocks[i], {
      code: 'ERR_INVALID_ARG_VALUE',
      name: 'TypeError',
    });
  }
}

// Test valid combinations of connect(port) and connect(port, host)
{
  var expectedConnections = 72;
  var serverConnected = 0;

  var server = net.createServer(common.mustCall(function(socket) {
    socket.end('ok');
    if (++serverConnected === expectedConnections) {
      server.close();
    }
  }, expectedConnections));

  server.listen(0, common.localhostIPv4, common.mustCall(function() {
    var port = server.address().port;

    // Total connections = 3 * 4(canConnect) * 6(doConnect) = 72
    canConnect(port);
    canConnect(String(port));
    canConnect('0x' + port.toString(16));
  }));

  // Try connecting to random ports, but do so once the server is closed
  server.on('close', function() {
    asyncFailToConnect(0);
  });
}

function doConnect(args, getCb) {
  return [
    function createConnectionWithCb() {
      return net.createConnection.apply(net, args.concat(getCb()))
        .resume();
    },
    function createConnectionWithoutCb() {
      return net.createConnection.apply(net, args)
        .on('connect', getCb())
        .resume();
    },
    function connectWithCb() {
      return net.connect.apply(net, args.concat(getCb()))
        .resume();
    },
    function connectWithoutCb() {
      return net.connect.apply(net, args)
        .on('connect', getCb())
        .resume();
    },
    function socketConnectWithCb() {
      var socket = new net.Socket();
      return socket.connect.apply(socket, args.concat(getCb()))
        .resume();
    },
    function socketConnectWithoutCb() {
      var socket = new net.Socket();
      return socket.connect.apply(socket, args)
        .on('connect', getCb())
        .resume();
    },
  ];
}

function syncFailToConnect(port, assertErr, optOnly) {
  var family = 4;
  if (!optOnly) {
    // connect(port, cb) and connect(port)
    var portArgFunctions = doConnect([{ port: port, family: family }],
                                     function() { return common.mustNotCall(); });
    for (var i = 0; i < portArgFunctions.length; i++) {
      assert.throws(portArgFunctions[i], assertErr,
                    portArgFunctions[i].name + '(' + port + ')');
    }

    // connect(port, host, cb) and connect(port, host)
    var portHostArgFunctions = doConnect([{ port: port,
                                            host: 'localhost',
                                            family: family }],
                                         function() { return common.mustNotCall(); });
    for (var j = 0; j < portHostArgFunctions.length; j++) {
      assert.throws(portHostArgFunctions[j], assertErr,
                    portHostArgFunctions[j].name + '(' + port + ", 'localhost')");
    }
  }
  // connect({port}, cb) and connect({port})
  var portOptFunctions = doConnect([{ port: port, family: family }],
                                   function() { return common.mustNotCall(); });
  for (var k = 0; k < portOptFunctions.length; k++) {
    assert.throws(portOptFunctions[k], assertErr,
                  portOptFunctions[k].name + '({port: ' + port + '})');
  }

  // connect({port, host}, cb) and connect({port, host})
  var portHostOptFunctions = doConnect([{ port: port,
                                          host: 'localhost',
                                          family: family }],
                                       function() { return common.mustNotCall(); });
  for (var l = 0; l < portHostOptFunctions.length; l++) {
    assert.throws(portHostOptFunctions[l],
                  assertErr,
                  portHostOptFunctions[l].name + '({port: ' + port + ", host: 'localhost'})");
  }
}

function canConnect(port) {
  var noop = function() { return common.mustCall(); };
  var family = 4;

  // connect(port, cb) and connect(port)
  var portArgFunctions = doConnect([{ port: port, family: family }], noop);
  for (var i = 0; i < portArgFunctions.length; i++) {
    portArgFunctions[i]();
  }

  // connect(port, host, cb) and connect(port, host)
  var portHostArgFunctions = doConnect([{ port: port,
                                          host: 'localhost',
                                          family: family }], noop);
  for (var j = 0; j < portHostArgFunctions.length; j++) {
    portHostArgFunctions[j]();
  }

  // connect({port}, cb) and connect({port})
  var portOptFunctions = doConnect([{ port: port, family: family }], noop);
  for (var k = 0; k < portOptFunctions.length; k++) {
    portOptFunctions[k]();
  }

  // connect({port, host}, cb) and connect({port, host})
  var portHostOptFns = doConnect([{ port: port,
                                    host: 'localhost',
                                    family: family }], noop);
  for (var l = 0; l < portHostOptFns.length; l++) {
    portHostOptFns[l]();
  }
}

function asyncFailToConnect(port) {
  var onError = function() {
    return common.mustCall(function(err) {
      var regexp = /^Error: connect E\w+.+$/;
      assert.match(String(err), regexp);
    });
  };

  var dont = function() { return common.mustNotCall(); };
  var family = 4;

  // connect(port, cb) and connect(port)
  var portArgFunctions = doConnect([{ port: port, family: family }], dont);
  for (var i = 0; i < portArgFunctions.length; i++) {
    portArgFunctions[i]().on('error', onError());
  }

  // connect({port}, cb) and connect({port})
  var portOptFunctions = doConnect([{ port: port, family: family }], dont);
  for (var j = 0; j < portOptFunctions.length; j++) {
    portOptFunctions[j]().on('error', onError());
  }

  // connect({port, host}, cb) and connect({port, host})
  var portHostOptFns = doConnect([{ port: port,
                                    host: 'localhost',
                                    family: family }], dont);
  for (var k = 0; k < portHostOptFns.length; k++) {
    portHostOptFns[k]().on('error', onError());
  }
}
