// Ported from Node.js test/parallel/test-http-keep-alive.js
// Copyright Joyent, Inc. and other Node contributors. MIT license.
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
var common = require('../common');
var assert = require('assert');
var http = require('http');

var body = 'hello world\n';

var server = http.createServer(common.mustCall(function(req, res) {
  res.writeHead(200, { 'Content-Length': body.length });
  res.write(body);
  res.end();
}, 3));

var agent = new http.Agent({ maxSockets: 1 });
var headers = { 'connection': 'keep-alive' };
var name;

server.listen(0, common.mustCall(function() {
  name = agent.getName({ port: this.address().port });
  http.get({
    path: '/', headers: headers, port: this.address().port, agent: agent
  }, common.mustCall(function(response) {
    assert.strictEqual(agent.sockets[name].length, 1);
    assert.strictEqual(agent.requests[name].length, 2);
    response.resume();
  }));

  http.get({
    path: '/', headers: headers, port: this.address().port, agent: agent
  }, common.mustCall(function(response) {
    assert.strictEqual(agent.sockets[name].length, 1);
    assert.strictEqual(agent.requests[name].length, 1);
    response.resume();
  }));

  http.get({
    path: '/', headers: headers, port: this.address().port, agent: agent
  }, common.mustCall(function(response) {
    response.on('end', common.mustCall(function() {
      assert.strictEqual(agent.sockets[name].length, 1);
      assert.ok(!(name in agent.requests));
      server.close();
    }));
    response.resume();
  }));
}));

process.on('exit', function() {
  assert.ok(!(name in agent.sockets));
  assert.ok(!(name in agent.requests));
});
