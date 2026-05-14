// Copyright (c) Tzvetan Mikov.
//
// Verifies server.headersTimeout: a connection that sends a partial
// request line + headers (and stops) is closed by the periodic
// checkConnections() loop, which calls ConnectionsList.expired() to
// find parsers that have been mid-headers longer than the deadline.
//
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

'use strict';

var assert = require('assert');
var http = require('http');
var net = require('net');

var server = http.createServer({
  // Aggressive timeouts so the test runs fast.
  headersTimeout: 200,
  requestTimeout: 0,
  connectionsCheckingInterval: 50,
}, function(req, res) {
  // Should never get here -- the request never completes its headers.
  res.writeHead(500);
  res.end('unexpected');
});

server.listen(0, function() {
  var port = server.address().port;

  var sock = net.connect(port, '127.0.0.1', function() {
    // Send a partial request line + part of the Host header. Crucially,
    // do NOT send the terminating "\r\n\r\n", so the parser stays in
    // headers-reading state with headersCompleted_ == false.
    sock.write('GET / HTTP/1.1\r\nHost:');

    var chunks = [];
    sock.on('data', function(c) { chunks.push(c); });

    var closed = false;
    sock.on('close', function() { closed = true; });

    // Wait long enough for the checkConnections interval to fire AFTER
    // the headersTimeout deadline passes (200ms timeout + 50ms interval
    // + slack).
    setTimeout(function() {
      var body = Buffer.concat(chunks).toString();
      assert.ok(closed, 'socket should have been closed by the server');
      assert.ok(
        body.indexOf('408') !== -1,
        'expected 408 Request Timeout, got: ' + JSON.stringify(body));

      server.close(function() {
        console.log('PASS');
      });
    }, 500);
  });
});
