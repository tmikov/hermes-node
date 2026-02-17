// Ported from Node.js test/parallel/test-http-write-empty-string.js
// Copyright Joyent, Inc. and other Node contributors. MIT license.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
var common = require('../common');
var assert = require('assert');

var http = require('http');

var server = http.createServer(function(request, response) {
  response.writeHead(200, { 'Content-Type': 'text/plain' });
  response.write('1\n');
  response.write('');
  response.write('2\n');
  response.write('');
  response.end('3\n');

  this.close();
});

server.listen(0, common.mustCall(function() {
  http.get({ port: server.address().port }, common.mustCall(function(res) {
    var response = '';

    assert.strictEqual(res.statusCode, 200);
    res.setEncoding('ascii');
    res.on('data', function(chunk) {
      response += chunk;
    });
    res.on('end', common.mustCall(function() {
      assert.strictEqual(response, '1\n2\n3\n');
    }));
  }));
}));
