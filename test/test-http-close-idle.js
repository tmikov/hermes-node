// Copyright (c) Tzvetan Mikov.
//
// Verifies server.closeIdleConnections() destroys only keep-alive
// sockets between requests, leaving mid-request sockets alone. Exercises
// the active/idle distinction in the ConnectionsList binding.
//
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

'use strict';

var assert = require('assert');
var http = require('http');
var net = require('net');

var server = http.createServer(function(req, res) {
  // Read the body fully, then respond.
  var chunks = [];
  req.on('data', function(c) { chunks.push(c); });
  req.on('end', function() {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('ok');
  });
});

server.listen(0, function() {
  var port = server.address().port;

  // Phase 1: open a keep-alive connection, complete one request, then
  // leave it idle.
  var agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
  var idleClientSocket = null;
  var idleClientClosed = false;

  var req = http.get({ port: port, agent: agent, path: '/idle' });
  req.on('socket', function(sock) {
    idleClientSocket = sock;
    sock.on('close', function() { idleClientClosed = true; });
  });
  req.on('response', function(res) {
    res.resume();
    res.on('end', function() {
      assert.ok(idleClientSocket, 'captured socket from socket event');

      // Phase 2: open a raw TCP connection and send only request headers
      // + a partial body. The parser is now mid-request: it has called
      // on_message_begin (lastMessageStart_ != 0) and is waiting for the
      // rest of the body.
      var midReqSocket = net.connect(port, '127.0.0.1', function() {
        var headers =
          'POST /slow HTTP/1.1\r\n' +
          'Host: localhost\r\n' +
          'Content-Length: 40\r\n' +
          'Connection: close\r\n' +
          '\r\n' +
          '01234567890123456789'; // 20 bytes of 40-byte body
        midReqSocket.write(headers);

        // Give the server a tick to register the parser in active_.
        setTimeout(function() {
          var midReqClosed = false;
          midReqSocket.on('close', function() { midReqClosed = true; });

          server.closeIdleConnections();

          // After a short delay: idle keep-alive socket destroyed,
          // mid-request socket untouched.
          setTimeout(function() {
            assert.strictEqual(
              idleClientClosed, true,
              'idle keep-alive socket should be destroyed by closeIdle');
            assert.strictEqual(
              midReqClosed, false,
              'mid-request socket must NOT be destroyed by closeIdle');

            // Now send the rest of the body. Server completes request,
            // sends response, then closes (Connection: close). After
            // that, server.close() can complete.
            var responseChunks = [];
            midReqSocket.on('data', function(c) { responseChunks.push(c); });

            midReqSocket.write('abcdefghijabcdefghij'); // last 20 bytes

            var serverClosed = false;
            server.close(function() { serverClosed = true; });

            setTimeout(function() {
              agent.destroy();
              if (!midReqSocket.destroyed) midReqSocket.destroy();
              assert.strictEqual(
                serverClosed, true,
                'server.close() should complete after request finishes');
              var body = Buffer.concat(responseChunks).toString();
              assert.ok(
                body.indexOf('200') !== -1,
                'expected 200 response, got: ' + body);
              console.log('PASS');
            }, 300);
          }, 100);
        }, 50);
      });
    });
  });
});
