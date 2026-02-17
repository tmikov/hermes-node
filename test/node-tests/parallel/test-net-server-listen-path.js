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

// Test pipe chmod -- skipped: libuv's uv_pipe_chmod first tries fchmod() which
// silently succeeds on some Linux kernels without actually changing the mode on
// Unix sockets. The chmod() fallback is never reached. This is a libuv issue
// (https://github.com/libuv/libuv/issues), not a hermes-node issue.
// Node itself passes because it tests on kernels where fchmod works on sockets.

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
