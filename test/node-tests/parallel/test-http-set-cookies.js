// Ported from Node.js test/parallel/test-http-set-cookies.js
// Copyright Joyent, Inc. and other Node contributors. MIT license.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
require('../common');
var assert = require('assert');
var http = require('http');
var Countdown = require('../common/countdown');

var countdown = new Countdown(2, function() { server.close(); });
var server = http.createServer(function(req, res) {
  if (req.url === '/one') {
    res.writeHead(200, [['set-cookie', 'A'],
                        ['content-type', 'text/plain']]);
    res.end('one\n');
  } else {
    res.writeHead(200, [['set-cookie', 'A'],
                        ['set-cookie', 'B'],
                        ['content-type', 'text/plain']]);
    res.end('two\n');
  }
});
server.listen(0);

server.on('listening', function() {
  //
  // one set-cookie header
  //
  http.get({ port: this.address().port, path: '/one' }, function(res) {
    // set-cookie headers are always return in an array.
    // even if there is only one.
    assert.deepStrictEqual(res.headers['set-cookie'], ['A']);
    assert.strictEqual(res.headers['content-type'], 'text/plain');

    res.on('data', function(chunk) {
      // consume
    });

    res.on('end', function() {
      countdown.dec();
    });
  });

  // Two set-cookie headers

  http.get({ port: this.address().port, path: '/two' }, function(res) {
    assert.deepStrictEqual(res.headers['set-cookie'], ['A', 'B']);
    assert.strictEqual(res.headers['content-type'], 'text/plain');

    res.on('data', function(chunk) {
      // consume
    });

    res.on('end', function() {
      countdown.dec();
    });
  });

});
