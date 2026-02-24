// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s

// Verify vendored ws package: require('node:ws') and require('ws') both
// resolve to the embedded vendored ws. Creates a server, connects a client,
// exchanges a message.
// Uses exit-code-based testing (not FileCheck pipe) because async cleanup
// can race with SIGPIPE.

'use strict';

var assert = require('assert');

// Test 1: require('node:ws') returns the vendored ws.
var wsFromNodePrefix = require('node:ws');
assert(typeof wsFromNodePrefix.WebSocket === 'function',
  'require("node:ws").WebSocket should be a function');
assert(typeof wsFromNodePrefix.WebSocketServer === 'function',
  'require("node:ws").WebSocketServer should be a function');

// Test 2: require('ws') with no user node_modules falls back to vendored.
var wsFromBare = require('ws');
assert(typeof wsFromBare.WebSocket === 'function',
  'require("ws").WebSocket should be a function');
assert(wsFromBare === wsFromNodePrefix,
  'require("ws") and require("node:ws") should return the same module');

// Test 3: WebSocket server and client communication.
var WebSocketServer = wsFromNodePrefix.WebSocketServer;
var WebSocket = wsFromNodePrefix.WebSocket;

var serverReceivedMessage = false;
var clientReceivedMessage = false;

var wss = new WebSocketServer({ port: 0 }); // random port

wss.on('listening', function() {
  var port = wss.address().port;

  wss.on('connection', function(ws) {
    ws.on('message', function(data) {
      serverReceivedMessage = true;
      assert(data.toString() === 'hello from client',
        'Server should receive "hello from client"');
      ws.send('hello from server');
    });
  });

  var client = new WebSocket('ws://127.0.0.1:' + port);

  client.on('open', function() {
    client.send('hello from client');
  });

  client.on('message', function(data) {
    clientReceivedMessage = true;
    assert(data.toString() === 'hello from server',
      'Client should receive "hello from server"');
    client.close();
  });

  client.on('close', function() {
    wss.close();
  });
});

wss.on('close', function() {
  assert(serverReceivedMessage, 'Server should have received a message');
  assert(clientReceivedMessage, 'Client should have received a message');
  console.log('PASS');
});
