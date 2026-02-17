// Ported from Node.js test/parallel/test-net-bytes-read.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';

var common = require('../common');
var assert = require('assert');
var net = require('net');

// 256KB: compromise between 1MB (too slow under ASAN) and 64KB (too small
// to stress multi-chunk reads).
var big = Buffer.alloc(256 * 1024);

var handler = common.mustCall(function(socket) {
  socket.end(big);
  server.close();
});

var onListen = common.mustCall(function() {
  var prev = 0;

  function checkRaise(value) {
    assert(value > prev);
    prev = value;
  }

  var onData = common.mustCallAtLeast(function(chunk) {
    checkRaise(socket.bytesRead);
  });

  var onEnd = common.mustCall(function() {
    assert.strictEqual(socket.bytesRead, prev);
    assert.strictEqual(big.length, prev);
  });

  var onClose = common.mustCall(function() {
    assert(!socket._handle);
    assert.strictEqual(socket.bytesRead, prev);
    assert.strictEqual(big.length, prev);
  });

  var onConnect = common.mustCall(function() {
    socket.on('data', onData);
    socket.on('end', onEnd);
    socket.on('close', onClose);
    socket.end();
  });

  var socket = net.connect(server.address().port, onConnect);
});

var server = net.createServer(handler).listen(0, onListen);
