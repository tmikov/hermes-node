// Ported from Node.js test/parallel/test-net-server-listen-path.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';

var common = require('../common');
var net = require('net');
var assert = require('assert');
var fs = require('fs');

var tmpdir = require('../common/tmpdir');
tmpdir.refresh();

function closeServer() {
  return common.mustCall(function() {
    this.close();
  });
}

var counter = 0;

function randomPipePath() {
  return common.PIPE + '-listen-path-' + (counter++);
}

// Test listen(path)
{
  var handlePath = randomPipePath();
  net.createServer()
    .listen(handlePath)
    .on('listening', closeServer());
}

// Test listen({path})
{
  var handlePath = randomPipePath();
  net.createServer()
    .listen({ path: handlePath })
    .on('listening', closeServer());
}

// Test listen(path, cb)
{
  var handlePath = randomPipePath();
  net.createServer()
    .listen(handlePath, closeServer());
}

// Test listen({path}, cb)
{
  var handlePath = randomPipePath();
  net.createServer()
    .listen({ path: handlePath }, closeServer());
}

// Test pipe chmod with readableAll/writableAll.
// On some Linux kernels, libuv's fchmod() silently succeeds on Unix sockets
// without actually changing the mode. Only assert if chmod actually worked.
{
  var handlePath = randomPipePath();
  var server = net.createServer()
    .listen({
      path: handlePath,
      readableAll: true,
      writableAll: true
    }, common.mustCall(function() {
      if (process.platform !== 'win32') {
        var mode = fs.statSync(handlePath).mode;
        // Check if fchmod actually worked by looking at S_IWOTH, which is
        // NOT set by default (umask 022 gives rwxr-xr-x). If fchmod
        // silently succeeded without effect, S_IWOTH will be 0 and we skip.
        if ((mode & fs.constants.S_IWOTH) !== 0) {
          assert.notStrictEqual(mode & fs.constants.S_IROTH, 0);
        }
      }
      server.close();
    }));
}

// Test should emit "error" events when listening fails.
{
  var handlePath = randomPipePath();
  var server1 = net.createServer().listen({ path: handlePath }, function() {
    // As the handlePath is in use, binding to the same address again should
    // make the server emit an 'EADDRINUSE' error.
    var server2 = net.createServer()
      .listen({
        path: handlePath,
        writableAll: true,
      }, common.mustNotCall());

    server2.on('error', common.mustCall(function(err) {
      server1.close();
      assert.strictEqual(err.code, 'EADDRINUSE');
      assert.match(err.message, /^listen EADDRINUSE: address already in use/);
    }));
  });
}
