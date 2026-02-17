// Test tcp_wrap binding.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

'use strict';

var assert = require('assert');

// Test 1: Verify tcp_wrap binding exports.
var tcpWrap = internalBinding('tcp_wrap');
assert(typeof tcpWrap.TCP === 'function', 'TCP constructor exists');
assert(typeof tcpWrap.TCPConnectWrap === 'function', 'TCPConnectWrap constructor exists');
assert(typeof tcpWrap.constants === 'object', 'constants exists');
assert(typeof tcpWrap.constants.SOCKET === 'number', 'SOCKET constant');
assert(typeof tcpWrap.constants.SERVER === 'number', 'SERVER constant');
assert(typeof tcpWrap.constants.UV_TCP_IPV6ONLY === 'number', 'UV_TCP_IPV6ONLY');

// Test 2: Create TCP handle and check basic methods.
var TCP = tcpWrap.TCP;
var handle = new TCP(tcpWrap.constants.SOCKET);
assert(typeof handle.bind === 'function', 'bind exists');
assert(typeof handle.bind6 === 'function', 'bind6 exists');
assert(typeof handle.listen === 'function', 'listen exists');
assert(typeof handle.connect === 'function', 'connect exists');
assert(typeof handle.connect6 === 'function', 'connect6 exists');
assert(typeof handle.getsockname === 'function', 'getsockname exists');
assert(typeof handle.getpeername === 'function', 'getpeername exists');
assert(typeof handle.setNoDelay === 'function', 'setNoDelay exists');
assert(typeof handle.setKeepAlive === 'function', 'setKeepAlive exists');
assert(typeof handle.reset === 'function', 'reset exists');
// Stream methods inherited from LibuvStreamBase
assert(typeof handle.readStart === 'function', 'readStart exists');
assert(typeof handle.readStop === 'function', 'readStop exists');
assert(typeof handle.writeBuffer === 'function', 'writeBuffer exists');
assert(typeof handle.writeUtf8String === 'function', 'writeUtf8String exists');
assert(typeof handle.shutdown === 'function', 'shutdown exists');
// HandleWrap methods
assert(typeof handle.close === 'function', 'close exists');
assert(typeof handle.ref === 'function', 'ref exists');
assert(typeof handle.unref === 'function', 'unref exists');
assert(typeof handle.hasRef === 'function', 'hasRef exists');
handle.close();

// Test 3: Bind and getsockname.
var server = new TCP(tcpWrap.constants.SERVER);
var err = server.bind('127.0.0.1', 0, 0);
assert.strictEqual(err, 0, 'bind should succeed');

var out = {};
err = server.getsockname(out);
assert.strictEqual(err, 0, 'getsockname should succeed');
assert.strictEqual(out.address, '127.0.0.1', 'bound to 127.0.0.1');
assert.strictEqual(out.family, 'IPv4', 'family is IPv4');
assert(typeof out.port === 'number' && out.port > 0, 'got a port');
var boundPort = out.port;

// Test 4: Listen and connect, verify data flow.
var TCPConnectWrap = tcpWrap.TCPConnectWrap;

err = server.listen(1);
assert.strictEqual(err, 0, 'listen should succeed');

var clientDataReceived = '';
var serverSawConnection = false;

server.onconnection = function(status, clientHandle) {
  assert.strictEqual(status, 0, 'onconnection status 0');
  serverSawConnection = true;

  // Write data to client.
  var ShutdownWrap = internalBinding('stream_wrap').ShutdownWrap;
  var WriteWrap = internalBinding('stream_wrap').WriteWrap;

  var writeReq = new WriteWrap();
  writeReq.oncomplete = function(writeStatus) {
    // After write completes, shutdown the server side.
    var shutReq = new ShutdownWrap();
    shutReq.oncomplete = function() {
      clientHandle.close();
    };
    clientHandle.shutdown(shutReq);
  };
  clientHandle.writeUtf8String(writeReq, 'hello-tcp');
};

// Create client and connect.
var client = new TCP(tcpWrap.constants.SOCKET);
var connectReq = new TCPConnectWrap();
connectReq.oncomplete = function(status, handle, req, readable, writable) {
  assert.strictEqual(status, 0, 'connect oncomplete status 0');
  assert.strictEqual(readable, true, 'readable');
  assert.strictEqual(writable, true, 'writable');

  // Start reading.
  client.onread = function(ab) {
    var streamState = internalBinding('stream_wrap').streamBaseState;
    var nread = streamState[0]; // kReadBytesOrError
    if (nread > 0) {
      var buf = Buffer.from(ab);
      clientDataReceived += buf.toString('utf8');
    } else if (nread < 0) {
      // EOF or error - close client and server.
      client.close();
      server.close(function() {
        assert.strictEqual(clientDataReceived, 'hello-tcp', 'received data matches');
        assert(serverSawConnection, 'server saw connection');
        console.log('PASS');
      });
    }
  };
  client.readStart();
};

err = client.connect(connectReq, '127.0.0.1', boundPort);
assert.strictEqual(err, 0, 'connect should succeed');
