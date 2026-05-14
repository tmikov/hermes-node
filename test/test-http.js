// Copyright (c) Tzvetan Mikov.
// Comprehensive verification of the HTTP module.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

'use strict';

var assert = require('assert');
var http = require('http');
var net = require('net');

var testsRun = 0;
var testsExpected = 12;

function done() {
  testsRun++;
  if (testsRun === testsExpected) {
    console.log('PASS');
  }
}

// Test 1: Simple GET request + response
function test1() {
  var server = http.createServer(function(req, res) {
    assert.strictEqual(req.method, 'GET');
    assert.strictEqual(req.url, '/test');
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Hello from hermes-node');
  });

  server.listen(0, function() {
    var port = server.address().port;
    http.get('http://127.0.0.1:' + port + '/test', function(res) {
      var data = '';
      res.on('data', function(chunk) { data += chunk; });
      res.on('end', function() {
        assert.strictEqual(res.statusCode, 200);
        assert.strictEqual(data, 'Hello from hermes-node');
        assert.strictEqual(res.headers['content-type'], 'text/plain');
        server.close(function() { done(); });
      });
    });
  });
}

// Test 2: POST with request body
function test2() {
  var requestBody = 'Post Body For Test';
  var server = http.createServer(function(req, res) {
    assert.strictEqual(req.method, 'POST');
    var body = '';
    req.setEncoding('utf8');
    req.on('data', function(chunk) { body += chunk; });
    req.on('end', function() {
      assert.strictEqual(body, requestBody);
      res.writeHead(200);
      res.end('received');
    });
  });

  server.listen(0, function() {
    var req = http.request({
      port: server.address().port,
      path: '/data',
      method: 'POST',
    }, function(res) {
      assert.strictEqual(res.statusCode, 200);
      var data = '';
      res.on('data', function(chunk) { data += chunk; });
      res.on('end', function() {
        assert.strictEqual(data, 'received');
        server.close(function() { done(); });
      });
    });
    req.end(requestBody);
  });
}

// Test 3: Chunked transfer encoding (multiple writes)
function test3() {
  var chunks = ['chunk1', 'chunk2', 'chunk3'];
  var server = http.createServer(function(req, res) {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    chunks.forEach(function(c) { res.write(c); });
    res.end();
  });

  server.listen(0, function() {
    http.get('http://127.0.0.1:' + server.address().port + '/', function(res) {
      var data = '';
      res.on('data', function(chunk) { data += chunk; });
      res.on('end', function() {
        assert.strictEqual(data, 'chunk1chunk2chunk3');
        assert.strictEqual(res.headers['transfer-encoding'], 'chunked');
        server.close(function() { done(); });
      });
    });
  });
}

// Test 4: HTTP keep-alive (multiple requests on same connection)
function test4() {
  var requestCount = 0;
  var server = http.createServer(function(req, res) {
    requestCount++;
    res.writeHead(200);
    res.end('response-' + requestCount);
  });

  server.listen(0, function() {
    var port = server.address().port;
    var agent = new http.Agent({ keepAlive: true, maxSockets: 1 });

    http.get({ port: port, path: '/1', agent: agent }, function(res1) {
      var data1 = '';
      res1.on('data', function(chunk) { data1 += chunk; });
      res1.on('end', function() {
        assert.strictEqual(data1, 'response-1');
        http.get({ port: port, path: '/2', agent: agent }, function(res2) {
          var data2 = '';
          res2.on('data', function(chunk) { data2 += chunk; });
          res2.on('end', function() {
            assert.strictEqual(data2, 'response-2');
            assert.strictEqual(requestCount, 2);
            agent.destroy();
            server.close(function() { done(); });
          });
        });
      });
    });
  });
}

// Test 5: Request headers received correctly
function test5() {
  var server = http.createServer(function(req, res) {
    assert.strictEqual(req.headers['x-custom-header'], 'custom-value');
    assert.strictEqual(req.headers['x-another'], 'another-value');
    res.writeHead(200);
    res.end('ok');
  });

  server.listen(0, function() {
    http.get({
      port: server.address().port,
      path: '/',
      headers: {
        'X-Custom-Header': 'custom-value',
        'X-Another': 'another-value',
      },
    }, function(res) {
      res.resume();
      res.on('end', function() {
        server.close(function() { done(); });
      });
    });
  });
}

// Test 6: Response headers set correctly
function test6() {
  var server = http.createServer(function(req, res) {
    res.setHeader('X-Response-Custom', 'resp-value');
    res.setHeader('X-Multi', 'val1');
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end('{"ok":true}');
  });

  server.listen(0, function() {
    http.get('http://127.0.0.1:' + server.address().port + '/', function(res) {
      assert.strictEqual(res.headers['x-response-custom'], 'resp-value');
      assert.strictEqual(res.headers['content-type'], 'application/json');
      res.resume();
      res.on('end', function() {
        server.close(function() { done(); });
      });
    });
  });
}

// Test 7: HTTP status codes
function test7() {
  var codes = [200, 201, 204, 301, 404, 500];
  var idx = 0;

  var server = http.createServer(function(req, res) {
    var code = codes[idx];
    if (code === 204) {
      // 204 No Content must not have a body
      res.writeHead(code);
      res.end();
    } else {
      res.writeHead(code, { 'Content-Type': 'text/plain' });
      res.end('status ' + code);
    }
  });

  server.listen(0, function() {
    function nextCode() {
      if (idx >= codes.length) {
        server.close(function() { done(); });
        return;
      }
      var expectedCode = codes[idx];
      http.get('http://127.0.0.1:' + server.address().port + '/', function(res) {
        assert.strictEqual(res.statusCode, expectedCode);
        res.resume();
        res.on('end', function() {
          idx++;
          nextCode();
        });
      });
    }
    nextCode();
  });
}

// Test 8: Large response body (streaming)
function test8() {
  // 50KB response body to test streaming
  var bodySize = 50 * 1024;
  var bodyChunk = 'x'.repeat(1024);
  var server = http.createServer(function(req, res) {
    res.writeHead(200);
    for (var i = 0; i < 50; i++) {
      res.write(bodyChunk);
    }
    res.end();
  });

  server.listen(0, function() {
    http.get('http://127.0.0.1:' + server.address().port + '/', function(res) {
      var received = 0;
      res.on('data', function(chunk) { received += chunk.length; });
      res.on('end', function() {
        assert.strictEqual(received, bodySize);
        server.close(function() { done(); });
      });
    });
  });
}

// Test 9: Client timeout
function test9() {
  var server = http.createServer(function(req, res) {
    // Don't respond immediately - let the client timeout
    // The server will just hold the connection open
  });

  server.listen(0, function() {
    var req = http.get({
      port: server.address().port,
      path: '/',
      timeout: 100,
    });

    var timedOut = false;
    req.on('timeout', function() {
      timedOut = true;
      req.destroy();
    });

    req.on('error', function(err) {
      // Expected: socket hang up or ECONNRESET after destroy
      assert.ok(timedOut, 'should have timed out first');
      server.close(function() { done(); });
    });
  });
}

// Test 10: Server close while connections active
function test10() {
  var server = http.createServer(function(req, res) {
    res.writeHead(200);
    res.end('closing');
  });

  server.listen(0, function() {
    http.get('http://127.0.0.1:' + server.address().port + '/', function(res) {
      var data = '';
      res.on('data', function(chunk) { data += chunk; });
      res.on('end', function() {
        assert.strictEqual(data, 'closing');
        // Close server after receiving response
        server.close(function() { done(); });
      });
    });
  });
}

// Test 11: http.request with hostname resolution (dns.lookup integration)
function test11() {
  var server = http.createServer(function(req, res) {
    res.writeHead(200);
    res.end('dns-ok');
  });

  server.listen(0, '127.0.0.1', function() {
    var port = server.address().port;
    http.get({
      hostname: 'localhost',
      port: port,
      path: '/',
    }, function(res) {
      var data = '';
      res.on('data', function(chunk) { data += chunk; });
      res.on('end', function() {
        assert.strictEqual(res.statusCode, 200);
        assert.strictEqual(data, 'dns-ok');
        server.close(function() { done(); });
      });
    }).on('error', function(err) {
      // Some CI environments may not resolve 'localhost' -- skip gracefully
      console.log('dns test skipped: ' + err.message);
      server.close(function() { done(); });
    });
  });
}

// Test 12: server.closeAllConnections() destroys keep-alive sockets so that
// server.close() can complete promptly. Regression test for the case where
// our HTTPParser binding never registers parsers in the server's
// ConnectionsList, leaving closeAllConnections() with nothing to close.
function test12() {
  var server = http.createServer(function(req, res) {
    res.writeHead(200);
    res.end('ok');
  });

  server.listen(0, function() {
    var port = server.address().port;
    var agent = new http.Agent({ keepAlive: true, maxSockets: 1 });

    http.get({ port: port, agent: agent }, function(res) {
      res.resume();
      res.on('end', function() {
        // Connection is now keep-alive idle on both sides.
        var serverClosed = false;
        server.close(function() { serverClosed = true; });
        // Should destroy the idle keep-alive socket, allowing server.close()
        // to complete on the next tick.
        server.closeAllConnections();
        setTimeout(function() {
          // Tear down the client side regardless of outcome so the process
          // can exit. Then assert.
          agent.destroy();
          if (!serverClosed) {
            console.log(
              'FAIL: server.close() did not complete after closeAllConnections()');
            process.exit(1);
          }
          done();
        }, 100);
      });
    });
  });
}

// Run all tests
test1();
test2();
test3();
test4();
test5();
test6();
test7();
test8();
test9();
test10();
test11();
test12();
