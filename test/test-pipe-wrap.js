// Test pipe_wrap binding.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

'use strict';

var assert = require('assert');
var path = require('path');
var os = require('os');
var fs = require('fs');

// Test 1: Verify pipe_wrap binding exports.
var pipeWrap = internalBinding('pipe_wrap');
assert(typeof pipeWrap.Pipe === 'function', 'Pipe constructor exists');
assert(typeof pipeWrap.PipeConnectWrap === 'function', 'PipeConnectWrap constructor exists');
assert(typeof pipeWrap.constants === 'object', 'constants exists');
assert.strictEqual(pipeWrap.constants.SOCKET, 0, 'SOCKET constant');
assert.strictEqual(pipeWrap.constants.SERVER, 1, 'SERVER constant');
assert.strictEqual(pipeWrap.constants.IPC, 2, 'IPC constant');
assert(typeof pipeWrap.constants.UV_READABLE === 'number', 'UV_READABLE');
assert(typeof pipeWrap.constants.UV_WRITABLE === 'number', 'UV_WRITABLE');

// Test 2: Create Pipe handle and check methods.
var Pipe = pipeWrap.Pipe;
var handle = new Pipe(pipeWrap.constants.SOCKET);
assert(typeof handle.open === 'function', 'open exists');
assert(typeof handle.bind === 'function', 'bind exists');
assert(typeof handle.listen === 'function', 'listen exists');
assert(typeof handle.connect === 'function', 'connect exists');
assert(typeof handle.fchmod === 'function', 'fchmod exists');
assert(typeof handle.getsockname === 'function', 'getsockname exists');
assert(typeof handle.getpeername === 'function', 'getpeername exists');
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

// Test 3: Bind, listen, connect, data flow via pipe.
var PipeConnectWrap = pipeWrap.PipeConnectWrap;

// Use a unique socket path based on PID and a test thread ID (for parallel lit).
var threadId = process.env.TEST_THREAD_ID || '';
var sockPath = path.join(
  os.tmpdir(),
  'hermes-pipe-test-' + process.pid + '-' + threadId + '.sock'
);

// Clean up any stale socket file.
try { fs.unlinkSync(sockPath); } catch(e) {}

var server = new Pipe(pipeWrap.constants.SERVER);
var err = server.bind(sockPath);
assert.strictEqual(err, 0, 'bind should succeed: ' + err);

// Verify getsockname returns the bound path.
var out = {};
err = server.getsockname(out);
assert.strictEqual(err, 0, 'getsockname should succeed');
assert.strictEqual(out.address, sockPath, 'getsockname returns bound path');

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
  clientHandle.writeUtf8String(writeReq, 'hello-pipe');
};

// Create client and connect.
var client = new Pipe(pipeWrap.constants.SOCKET);
var connectReq = new PipeConnectWrap();
connectReq.oncomplete = function(status, pipeHandle, req, readable, writable) {
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
        assert.strictEqual(clientDataReceived, 'hello-pipe', 'received data matches');
        assert(serverSawConnection, 'server saw connection');
        // Clean up the socket file.
        try { fs.unlinkSync(sockPath); } catch(e) {}
        console.log('PASS');
      });
    }
  };
  client.readStart();
};

client.connect(connectReq, sockPath);
