// Copyright (c) Tzvetan Mikov.
// Comprehensive verification of the net module.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

'use strict';

var assert = require('assert');
var net = require('net');
var path = require('path');
var os = require('os');
var fs = require('fs');
var dns = require('dns');

var pending = 0;

function done() {
  pending--;
  if (pending === 0) {
    console.log('PASS');
  }
}

function fail(msg) {
  console.error('FAIL: ' + msg);
  process.exit(1);
}

// ---------- Test 1: TCP echo server ----------
pending++;
(function testTcpEcho() {
  var server = net.createServer(function(socket) {
    socket.pipe(socket); // echo back
  });

  server.listen(0, function() {
    var port = server.address().port;
    var client = net.connect(port, '127.0.0.1', function() {
      client.write('hello-echo');
      client.end();
    });

    var data = '';
    client.on('data', function(chunk) { data += chunk; });
    client.on('end', function() {
      assert.strictEqual(data, 'hello-echo', 'Test 1: echo data matches');
      server.close(function() {
        done();
      });
    });
  });
})();

// ---------- Test 2: Multiple concurrent connections ----------
pending++;
(function testConcurrentConnections() {
  var conns = 5;
  var received = 0;

  var server = net.createServer(function(socket) {
    socket.on('data', function(chunk) {
      socket.write('reply-' + chunk.toString());
    });
    socket.on('end', function() {
      socket.end();
    });
  });

  server.listen(0, function() {
    var port = server.address().port;

    for (var i = 0; i < conns; i++) {
      (function(idx) {
        var client = net.connect(port, '127.0.0.1', function() {
          client.write('msg' + idx);
          client.end();
        });

        var data = '';
        client.on('data', function(chunk) { data += chunk; });
        client.on('end', function() {
          assert.strictEqual(data, 'reply-msg' + idx, 'Test 2: conn ' + idx + ' reply');
          received++;
          if (received === conns) {
            server.close(function() {
              done();
            });
          }
        });
      })(i);
    }
  });
})();

// ---------- Test 3: Server address() ----------
pending++;
(function testServerAddress() {
  var server = net.createServer();

  server.listen(0, '127.0.0.1', function() {
    var addr = server.address();
    assert.strictEqual(addr.address, '127.0.0.1', 'Test 3: address');
    assert.strictEqual(addr.family, 'IPv4', 'Test 3: family');
    assert(typeof addr.port === 'number' && addr.port > 0, 'Test 3: port');
    server.close(function() {
      done();
    });
  });
})();

// ---------- Test 4: Socket properties ----------
pending++;
(function testSocketProperties() {
  var server = net.createServer(function(socket) {
    // Server-side: check remoteAddress/remotePort
    assert.strictEqual(socket.remoteAddress, '127.0.0.1', 'Test 4: server remoteAddress');
    assert(typeof socket.remotePort === 'number', 'Test 4: server remotePort is number');
    socket.end();
  });

  server.listen(0, '127.0.0.1', function() {
    var port = server.address().port;
    var client = net.connect(port, '127.0.0.1', function() {
      // Client-side: check localAddress/localPort/remoteAddress/remotePort
      assert.strictEqual(client.remoteAddress, '127.0.0.1', 'Test 4: client remoteAddress');
      assert.strictEqual(client.remotePort, port, 'Test 4: client remotePort');
      assert.strictEqual(client.localAddress, '127.0.0.1', 'Test 4: client localAddress');
      assert(typeof client.localPort === 'number', 'Test 4: client localPort is number');
    });

    client.on('end', function() {
      client.end();
      server.close(function() {
        done();
      });
    });
  });
})();

// ---------- Test 5: setNoDelay, setKeepAlive ----------
pending++;
(function testSocketOptions() {
  var server = net.createServer(function(socket) {
    socket.end();
  });

  server.listen(0, function() {
    var port = server.address().port;
    var client = net.connect(port, '127.0.0.1', function() {
      // These should not throw
      client.setNoDelay(true);
      client.setNoDelay(false);
      client.setKeepAlive(true, 1000);
      client.setKeepAlive(false);
    });

    client.on('end', function() {
      client.end();
      server.close(function() {
        done();
      });
    });
  });
})();

// ---------- Test 6: socket.setTimeout ----------
pending++;
(function testSocketTimeout() {
  var server = net.createServer(function(socket) {
    // Don't send data -- let the client timeout
  });

  server.listen(0, function() {
    var port = server.address().port;
    var client = net.connect(port, '127.0.0.1', function() {
      var timedOut = false;
      client.setTimeout(100, function() {
        timedOut = true;
        client.destroy();
        server.close(function() {
          assert(timedOut, 'Test 6: timeout fired');
          done();
        });
      });
    });
  });
})();

// ---------- Test 7: net.isIP, net.isIPv4, net.isIPv6 ----------
pending++;
(function testIsIP() {
  assert.strictEqual(net.isIP('127.0.0.1'), 4, 'Test 7: isIP IPv4');
  assert.strictEqual(net.isIP('::1'), 6, 'Test 7: isIP IPv6');
  assert.strictEqual(net.isIP('not-an-ip'), 0, 'Test 7: isIP invalid');

  assert.strictEqual(net.isIPv4('127.0.0.1'), true, 'Test 7: isIPv4');
  assert.strictEqual(net.isIPv4('::1'), false, 'Test 7: isIPv4 on IPv6');

  assert.strictEqual(net.isIPv6('::1'), true, 'Test 7: isIPv6');
  assert.strictEqual(net.isIPv6('127.0.0.1'), false, 'Test 7: isIPv6 on IPv4');

  done();
})();

// ---------- Test 8: Error handling (ECONNREFUSED) ----------
pending++;
(function testEconnrefused() {
  // Connect to an unused port -- should get ECONNREFUSED
  var client = net.connect(1, '127.0.0.1');
  client.on('error', function(err) {
    assert.strictEqual(err.code, 'ECONNREFUSED', 'Test 8: ECONNREFUSED');
    done();
  });
})();

// ---------- Test 9: Pipe (Unix domain socket) ----------
pending++;
(function testPipe() {
  var threadId = process.env.TEST_THREAD_ID || '';
  var sockPath = path.join(
    os.tmpdir(),
    'hermes-net-test-' + process.pid + '-' + threadId + '.sock'
  );
  try { fs.unlinkSync(sockPath); } catch(e) {}

  var server = net.createServer(function(socket) {
    socket.write('pipe-hello');
    socket.end();
  });

  server.listen(sockPath, function() {
    var client = net.connect(sockPath, function() {
      var data = '';
      client.on('data', function(chunk) { data += chunk; });
      client.on('end', function() {
        assert.strictEqual(data, 'pipe-hello', 'Test 9: pipe data');
        client.end();
        server.close(function() {
          try { fs.unlinkSync(sockPath); } catch(e) {}
          done();
        });
      });
    });
  });
})();

// ---------- Test 10: dns.lookup integration (connect by hostname) ----------
pending++;
(function testHostnameLookup() {
  var server = net.createServer(function(socket) {
    socket.write('resolved');
    socket.end();
  });

  server.listen(0, '127.0.0.1', function() {
    var port = server.address().port;
    // Connect by hostname -- this triggers dns.lookup
    var client = net.connect(port, 'localhost', function() {
      var data = '';
      client.on('data', function(chunk) { data += chunk; });
      client.on('end', function() {
        assert.strictEqual(data, 'resolved', 'Test 10: hostname connect data');
        client.end();
        server.close(function() {
          done();
        });
      });
    });
  });
})();
