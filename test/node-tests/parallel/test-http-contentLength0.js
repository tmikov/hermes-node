// Ported from Node.js test/parallel/test-http-contentLength0.js
// Copyright Joyent, Inc. and other Node contributors. MIT license.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
require('../common');
var http = require('http');

// Simple test of Node's HTTP Client choking on a response
// with a 'Content-Length: 0 ' response header.
// I.E. a space character after the 'Content-Length' throws an `error` event.

var s = http.createServer(function(req, res) {
  res.writeHead(200, { 'Content-Length': '0 ' });
  res.end();
});
s.listen(0, function() {

  var request = http.request({ port: this.address().port }, function(response) {
    s.close();
    response.resume();
  });

  request.end();
});
