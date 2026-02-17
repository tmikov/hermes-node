// Ported from Node.js test/parallel/test-http-client-get-url.js
// Copyright Joyent, Inc. and other Node contributors. MIT license.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
var common = require('../common');
var assert = require('assert');
var http = require('http');
var url = require('url');
var testPath = '/foo?bar';

var server = http.createServer(common.mustCall(function(req, res) {
  assert.strictEqual(req.method, 'GET');
  assert.strictEqual(req.url, testPath);
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.write('hello\n');
  res.end();
}, 3));

server.listen(0, common.localhostIPv4, common.mustCall(function() {
  var u = 'http://' + common.localhostIPv4 + ':' + server.address().port + testPath;
  http.get(u, common.mustCall(function() {
    http.get(url.parse(u), common.mustCall(function() {
      http.get(new URL(u), common.mustCall(function() {
        server.close();
      }));
    }));
  }));
}));
