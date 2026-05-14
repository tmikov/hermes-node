// Copyright (c) Tzvetan Mikov.
//
// Verifies server.requestTimeout: a connection that finishes its
// headers but stalls partway through the body is closed by the periodic
// checkConnections() loop, which calls ConnectionsList.expired() and
// uses the requestTimeout branch (headersCompleted_ == true).
//
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

'use strict';

var assert = require('assert');
var http = require('http');
var net = require('net');

var requestStarted = false;

var server = http.createServer({
  headersTimeout: 0,
  requestTimeout: 200,
  connectionsCheckingInterval: 50,
}, function(req, res) {
  // Headers parsed; body never finishes -- the response should never be
  // sent because the request times out first.
  requestStarted = true;
  req.on('end', function() {
    res.writeHead(200);
    res.end('unexpected');
  });
});

server.listen(0, function() {
  var port = server.address().port;

  var sock = net.connect(port, '127.0.0.1', function() {
    // Complete request line + headers + partial body. The parser will
    // call on_headers_complete (setting headersCompleted_ = true) and
    // start reading the body, then stall.
    sock.write(
      'POST /slow HTTP/1.1\r\n' +
      'Host: localhost\r\n' +
      'Content-Length: 100\r\n' +
      '\r\n' +
      'partial-body-not-100-bytes',
    );

    var chunks = [];
    sock.on('data', function(c) { chunks.push(c); });

    var closed = false;
    sock.on('close', function() { closed = true; });

    setTimeout(function() {
      var body = Buffer.concat(chunks).toString();
      assert.ok(requestStarted, 'request handler should have been called');
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
