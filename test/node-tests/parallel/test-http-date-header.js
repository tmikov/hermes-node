// Ported from Node.js test/parallel/test-http-date-header.js
// Copyright Joyent, Inc. and other Node contributors. MIT license.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
require('../common');
var assert = require('assert');
var http = require('http');

var testResBody = 'other stuff!\n';

var server = http.createServer(function(req, res) {
  assert.ok(!('date' in req.headers),
            'Request headers contained a Date.');
  res.writeHead(200, {
    'Content-Type': 'text/plain'
  });
  res.end(testResBody);
});
server.listen(0);

server.addListener('listening', function() {
  var options = {
    port: server.address().port,
    path: '/',
    method: 'GET'
  };
  var req = http.request(options, function(res) {
    assert.ok('date' in res.headers,
              'Response headers didn\'t contain a Date.');
    res.addListener('end', function() {
      server.close();
    });
    res.resume();
  });
  req.end();
});
