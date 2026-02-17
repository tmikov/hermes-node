// Ported from Node.js test/parallel/test-http-request-end.js
// Copyright Joyent, Inc. and other Node contributors. MIT license.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
var common = require('../common');
var assert = require('assert');
var http = require('http');

var expected = 'Post Body For Test';
var expectedStatusCode = 200;

var server = http.Server(function(req, res) {
  var result = '';

  req.setEncoding('utf8');
  req.on('data', function(chunk) {
    result += chunk;
  });

  req.on('end', common.mustCall(function() {
    assert.strictEqual(result, expected);
    res.writeHead(expectedStatusCode);
    res.end('hello world\n');
    server.close();
  }));
});

server.listen(0, function() {
  var req = http.request({
    port: this.address().port,
    path: '/',
    method: 'POST'
  }, function(res) {
    assert.strictEqual(res.statusCode, expectedStatusCode);
    res.resume();
  }).on('error', common.mustNotCall());

  var result = req.end(expected);

  assert.strictEqual(req, result);
});
