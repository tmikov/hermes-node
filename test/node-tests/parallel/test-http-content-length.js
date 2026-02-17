// Ported from Node.js test/parallel/test-http-content-length.js
// Copyright Node.js contributors. MIT license.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
require('../common');
var assert = require('assert');
var http = require('http');
var Countdown = require('../common/countdown');

var expectedHeadersMultipleWrites = {
  'connection': 'keep-alive',
  'transfer-encoding': 'chunked',
};

var expectedHeadersEndWithData = {
  'connection': 'keep-alive',
  'content-length': String('hello world'.length),
};

var expectedHeadersEndNoData = {
  'connection': 'keep-alive',
  'content-length': '0',
};


var countdown = new Countdown(3, function() { server.close(); });

var server = http.createServer(function(req, res) {
  res.removeHeader('Date');
  res.setHeader('Keep-Alive', 'timeout=1');

  switch (req.url.slice(1)) {
    case 'multiple-writes':
      delete req.headers.host;
      assert.deepStrictEqual(req.headers, expectedHeadersMultipleWrites);
      res.write('hello');
      res.end('world');
      break;
    case 'end-with-data':
      delete req.headers.host;
      assert.deepStrictEqual(req.headers, expectedHeadersEndWithData);
      res.end('hello world');
      break;
    case 'empty':
      delete req.headers.host;
      assert.deepStrictEqual(req.headers, expectedHeadersEndNoData);
      res.end();
      break;
    default:
      throw new Error('Unreachable');
  }

  countdown.dec();
});

server.listen(0, function() {
  var req;

  req = http.request({
    port: this.address().port,
    method: 'POST',
    path: '/multiple-writes'
  });
  req.removeHeader('Date');
  req.write('hello ');
  req.end('world');
  req.on('response', function(res) {
    assert.deepStrictEqual(res.headers, Object.assign({}, expectedHeadersMultipleWrites, { 'keep-alive': 'timeout=1' }));
    res.resume();
  });

  req = http.request({
    port: this.address().port,
    method: 'POST',
    path: '/end-with-data'
  });
  req.removeHeader('Date');
  req.end('hello world');
  req.on('response', function(res) {
    assert.deepStrictEqual(res.headers, Object.assign({}, expectedHeadersEndWithData, { 'keep-alive': 'timeout=1' }));
    res.resume();
  });

  req = http.request({
    port: this.address().port,
    method: 'POST',
    path: '/empty'
  });
  req.removeHeader('Date');
  req.end();
  req.on('response', function(res) {
    assert.deepStrictEqual(res.headers, Object.assign({}, expectedHeadersEndNoData, { 'keep-alive': 'timeout=1' }));
    res.resume();
  });

});
