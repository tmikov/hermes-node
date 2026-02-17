// Ported from Node.js test/parallel/test-http-status-code.js
// Copyright Joyent, Inc. and other Node contributors. MIT license.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
require('../common');
var assert = require('assert');
var http = require('http');
var Countdown = require('../common/countdown');

// Simple test of Node's HTTP ServerResponse.statusCode
var tests = [200, 202, 300, 404, 451, 500];
var test;
var countdown = new Countdown(tests.length, function() { s.close(); });

var s = http.createServer(function(req, res) {
  res.writeHead(test, { 'Content-Type': 'text/plain' });
  assert.strictEqual(res.statusCode, test);
  res.end('hello world\n');
});

s.listen(0, nextTest);


function nextTest() {
  test = tests.shift();

  http.get({ port: s.address().port }, function(response) {
    assert.strictEqual(response.statusCode, test);
    response.on('end', function() {
      if (countdown.dec())
        nextTest();
    });
    response.resume();
  });
}
