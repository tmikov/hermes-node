// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s | %FileCheck %s
// CHECK: answered: 1.2.3.4
// CHECK: timeout: ETIMEOUT
// CHECK: PASS

// A c-ares query whose server never answers must still call back with
// ETIMEOUT, and a channel must neither keep a finished program alive nor let
// one with a pending query exit early. The second half is why the black-hole
// socket is unref'd: a ref'd socket would keep the loop alive by itself, and
// the test would then say nothing about the channel. Node v24 waits for
// ETIMEOUT with the socket unref'd, and so must this.
//
// The per-try timeout is deliberately above one second: the c-ares timeout
// timer used to be one-shot and capped at 1000 ms, so it fired once, before
// the deadline, and was never armed again -- the query was never expired and
// the process waited forever. A timeout under the cap happened to survive.
//
// No network is involved: both servers are local dgram sockets. Run time is
// bounded from inside the script, because macOS has no `timeout` binary.

'use strict';

var assert = require('assert');
var dgram = require('dgram');
var dns = require('dns');

var TIMEOUT_MS = 1100;
var TRIES = 2;

var watchdog = setTimeout(function () {
  console.log('FAIL: watchdog fired; a DNS callback never arrived');
  process.exit(1);
}, 20000);
// The watchdog must not be what keeps the process alive, or what ends it.
watchdog.unref();

// A server that answers every A query with 1.2.3.4.
function answeredCase(next) {
  var server = dgram.createSocket('udp4');
  server.on('message', function (msg, rinfo) {
    // Header: copy the id, set QR|RD|RA, QDCOUNT 1, ANCOUNT 1.
    var header = Buffer.from([
      msg[0], msg[1], 0x81, 0x80, 0, 1, 0, 1, 0, 0, 0, 0,
    ]);
    // The question section runs to the end of the query (no EDNS records
    // are expected, but stop at the first record after the question).
    var end = 12;
    while (msg[end] !== 0) end += msg[end] + 1;
    end += 5; // root label, QTYPE, QCLASS
    var question = msg.slice(12, end);
    var answer = Buffer.from([
      0xc0, 0x0c, // name: pointer to the question
      0, 1, 0, 1, // A, IN
      0, 0, 0, 60, // TTL
      0, 4, 1, 2, 3, 4,
    ]);
    server.send(
        Buffer.concat([header, question, answer]), rinfo.port, rinfo.address);
  });
  server.bind(0, '127.0.0.1', function () {
    var resolver = new dns.Resolver({timeout: TIMEOUT_MS, tries: TRIES});
    resolver.setServers(['127.0.0.1:' + server.address().port]);
    resolver.resolve4('example.com', function (err, addrs) {
      assert.ifError(err);
      assert.deepStrictEqual(addrs, ['1.2.3.4']);
      console.log('answered: ' + addrs[0]);
      server.close();
      next();
    });
  });
}

// A server that receives every query and never replies.
function timeoutCase() {
  var hole = dgram.createSocket('udp4');
  var received = 0;
  hole.on('message', function () {
    received++;
  });
  hole.bind(0, '127.0.0.1', function () {
    // Unref'd, so that the socket is not what keeps the process alive while
    // the query is pending: the channel must hold the loop by itself, as
    // Node's does, or the process ends before ETIMEOUT and PASS never
    // prints.
    hole.unref();
    var resolver = new dns.Resolver({timeout: TIMEOUT_MS, tries: TRIES});
    resolver.setServers(['127.0.0.1:' + hole.address().port]);
    var calls = 0;
    var start = Date.now();
    resolver.resolve4('example.com', function (err) {
      calls++;
      assert.strictEqual(calls, 1, 'callback called more than once');
      assert(err, 'expected an error');
      assert.strictEqual(err.code, 'ETIMEOUT');
      var elapsed = Date.now() - start;
      assert(
          received >= TRIES,
          'black hole received ' + received + ' datagrams, expected ' + TRIES);
      // c-ares must not report a timeout before the first try's deadline.
      assert(elapsed >= TIMEOUT_MS, 'timed out early: ' + elapsed + ' ms');
      console.log('timeout: ' + err.code);
      hole.close();
      // Nothing else is pending; the process must now end by itself. If any
      // handle still held the loop, the unref'd watchdog would fire and fail.
      process.on('exit', function () {
        assert.strictEqual(calls, 1);
        console.log('PASS');
      });
    });
  });
}

answeredCase(timeoutCase);
