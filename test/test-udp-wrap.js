// Test udp_wrap binding.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

'use strict';

var assert = require('assert');

// Test 1: Verify udp_wrap binding exports.
var udpWrap = internalBinding('udp_wrap');
assert(typeof udpWrap.UDP === 'function', 'UDP constructor exists');
assert(typeof udpWrap.SendWrap === 'function', 'SendWrap constructor exists');
assert(typeof udpWrap.constants === 'object', 'constants exists');
assert(typeof udpWrap.constants.UV_UDP_IPV6ONLY === 'number', 'UV_UDP_IPV6ONLY');
assert(typeof udpWrap.constants.UV_UDP_REUSEADDR === 'number', 'UV_UDP_REUSEADDR');
assert(typeof udpWrap.constants.UV_UDP_REUSEPORT === 'number', 'UV_UDP_REUSEPORT');

// Test 2: Create UDP handle, verify prototype methods.
var handle = new udpWrap.UDP();
assert(typeof handle.bind === 'function', 'bind method');
assert(typeof handle.bind6 === 'function', 'bind6 method');
assert(typeof handle.send === 'function', 'send method');
assert(typeof handle.send6 === 'function', 'send6 method');
assert(typeof handle.recvStart === 'function', 'recvStart method');
assert(typeof handle.recvStop === 'function', 'recvStop method');
assert(typeof handle.close === 'function', 'close method');
assert(typeof handle.ref === 'function', 'ref method');
assert(typeof handle.unref === 'function', 'unref method');
assert(typeof handle.getsockname === 'function', 'getsockname method');
assert(typeof handle.getpeername === 'function', 'getpeername method');
assert(typeof handle.connect === 'function', 'connect method');
assert(typeof handle.connect6 === 'function', 'connect6 method');
assert(typeof handle.disconnect === 'function', 'disconnect method');
assert(typeof handle.addMembership === 'function', 'addMembership method');
assert(typeof handle.dropMembership === 'function', 'dropMembership method');
assert(typeof handle.setMulticastTTL === 'function', 'setMulticastTTL method');
assert(typeof handle.setMulticastLoopback === 'function', 'setMulticastLoopback');
assert(typeof handle.setBroadcast === 'function', 'setBroadcast method');
assert(typeof handle.setTTL === 'function', 'setTTL method');
assert(typeof handle.bufferSize === 'function', 'bufferSize method');
assert(typeof handle.getSendQueueSize === 'function', 'getSendQueueSize method');
assert(typeof handle.getSendQueueCount === 'function', 'getSendQueueCount method');
handle.close();

// Test 3: Bind and getsockname.
(function testBindAndGetsockname() {
  var h = new udpWrap.UDP();
  var err = h.bind('0.0.0.0', 0, 0);
  assert.strictEqual(err, 0, 'bind should succeed: ' + err);

  var out = {};
  err = h.getsockname(out);
  assert.strictEqual(err, 0, 'getsockname should succeed');
  assert.strictEqual(out.address, '0.0.0.0', 'address should be 0.0.0.0');
  assert(typeof out.port === 'number' && out.port > 0, 'port should be assigned');
  assert.strictEqual(out.family, 'IPv4', 'family should be IPv4');
  h.close();
})();

// Test 4: UDP connect/disconnect.
(function testConnectDisconnect() {
  var h = new udpWrap.UDP();
  var bindErr = h.bind('127.0.0.1', 0, 0);
  assert.strictEqual(bindErr, 0, 'bind should succeed');

  var connectErr = h.connect('127.0.0.1', 12345);
  assert.strictEqual(connectErr, 0, 'connect should succeed');

  var peerOut = {};
  var peerErr = h.getpeername(peerOut);
  assert.strictEqual(peerErr, 0, 'getpeername should succeed after connect');
  assert.strictEqual(peerOut.address, '127.0.0.1', 'peer address');
  assert.strictEqual(peerOut.port, 12345, 'peer port');

  var disconnectErr = h.disconnect();
  assert.strictEqual(disconnectErr, 0, 'disconnect should succeed');
  h.close();
})();

// Test 5: UDP send and receive data.
(function testSendRecv() {
  var server = new udpWrap.UDP();
  var client = new udpWrap.UDP();

  var bindErr = server.bind('127.0.0.1', 0, 0);
  assert.strictEqual(bindErr, 0, 'server bind should succeed');

  var addrOut = {};
  server.getsockname(addrOut);
  var serverPort = addrOut.port;

  server.onmessage = function(nread, handle, buf, rinfo) {
    assert(nread > 0, 'nread should be positive: ' + nread);
    assert.strictEqual(buf.toString(), 'hello-udp', 'received data matches');
    assert.strictEqual(rinfo.address, '127.0.0.1', 'rinfo address');
    assert(typeof rinfo.port === 'number', 'rinfo port is number');
    assert.strictEqual(rinfo.family, 'IPv4', 'rinfo family');
    assert(typeof rinfo.size === 'number', 'rinfo size exists');

    server.recvStop();
    server.close();
    client.close();
    console.log('PASS');
  };

  var recvErr = server.recvStart();
  assert.strictEqual(recvErr, 0, 'recvStart should succeed');

  // Send data from client.
  var sendReq = new udpWrap.SendWrap();
  sendReq.oncomplete = function(status, bytesSent) {};

  var sendResult = client.send(
    sendReq, [Buffer.from('hello-udp')], 1, serverPort, '127.0.0.1', true);
  // sendResult > 0 means sync send (msgSize + 1), 0 means async.
  assert(sendResult >= 0, 'send should not fail: ' + sendResult);
})();
