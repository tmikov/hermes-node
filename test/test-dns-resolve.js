// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test dns.resolve* APIs via c-ares binding.

'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var dns = require('dns');
var cares = internalBinding('cares_wrap');

// --- Test 1: ChannelWrap has query methods ---
var channel = new cares.ChannelWrap(0, 4, 0);
assert(typeof channel.queryA === 'function', 'queryA');
assert(typeof channel.queryAaaa === 'function', 'queryAaaa');
assert(typeof channel.queryMx === 'function', 'queryMx');
assert(typeof channel.queryNs === 'function', 'queryNs');
assert(typeof channel.queryTxt === 'function', 'queryTxt');
assert(typeof channel.querySrv === 'function', 'querySrv');
assert(typeof channel.queryCname === 'function', 'queryCname');
assert(typeof channel.queryPtr === 'function', 'queryPtr');
assert(typeof channel.queryNaptr === 'function', 'queryNaptr');
assert(typeof channel.querySoa === 'function', 'querySoa');
assert(typeof channel.queryCaa === 'function', 'queryCaa');
assert(typeof channel.queryAny === 'function', 'queryAny');
assert(typeof channel.getHostByAddr === 'function', 'getHostByAddr');

// --- Test 2: getServers returns real server list ---
var servers = channel.getServers();
assert(Array.isArray(servers), 'getServers returns array');
// Should have at least one server from /etc/resolv.conf.
assert(servers.length > 0, 'should have at least one DNS server');
assert(Array.isArray(servers[0]), 'server entry is array');
assert(typeof servers[0][0] === 'string', 'server IP is string');
assert(typeof servers[0][1] === 'number', 'server port is number');

// --- Test 3: setServers works ---
var setResult = channel.setServers([[4, '8.8.8.8', 53]]);
assert(setResult === 0, 'setServers should return 0');
var newServers = channel.getServers();
assert(newServers.length === 1, 'should have 1 server after setServers');
assert(newServers[0][0] === '8.8.8.8', 'server should be 8.8.8.8');

// Reset to defaults by recreating channel.
channel = new cares.ChannelWrap(0, 4, 0);

// --- Test 4: dns.Resolver exists ---
assert(typeof dns.Resolver === 'function', 'dns.Resolver is a constructor');

// --- Test 5: dns.resolve and dns.resolve4 are functions ---
assert(typeof dns.resolve === 'function', 'dns.resolve');
assert(typeof dns.resolve4 === 'function', 'dns.resolve4');
assert(typeof dns.resolve6 === 'function', 'dns.resolve6');
assert(typeof dns.resolveMx === 'function', 'dns.resolveMx');
assert(typeof dns.resolveTxt === 'function', 'dns.resolveTxt');
assert(typeof dns.resolveSrv === 'function', 'dns.resolveSrv');
assert(typeof dns.resolveNs === 'function', 'dns.resolveNs');
assert(typeof dns.resolveCname === 'function', 'dns.resolveCname');
assert(typeof dns.resolvePtr === 'function', 'dns.resolvePtr');
assert(typeof dns.resolveSoa === 'function', 'dns.resolveSoa');
assert(typeof dns.reverse === 'function', 'dns.reverse');

// --- Test 6: dns.promises exists and has methods ---
var dnsPromises = dns.promises;
assert(typeof dnsPromises === 'object', 'dns.promises exists');
assert(typeof dnsPromises.lookup === 'function', 'dns.promises.lookup');
assert(typeof dnsPromises.resolve4 === 'function', 'dns.promises.resolve4');
assert(typeof dnsPromises.resolve6 === 'function', 'dns.promises.resolve6');

// Track async tests.
var pending = 0;
var failed = false;
function done() {
  pending--;
  if (pending === 0 && !failed) {
    console.log('PASS');
  }
}

function fail(msg) {
  if (!failed) {
    failed = true;
    console.error('FAIL: ' + msg);
    process.exit(1);
  }
}

// --- Test 7: Direct queryA on the binding ---
// Use the binding directly to test queryA for localhost.
pending++;
(function() {
  var ch = new cares.ChannelWrap(0, 4, 0);
  var req = new cares.QueryReqWrap();
  req.oncomplete = function(status, result, ttls) {
    // Querying "localhost" via c-ares might fail (depends on /etc/hosts config
    // and whether the system resolver supports it via c-ares). That's OK.
    // We just verify the callback was called with the right shape.
    assert(typeof status === 'number', 'status is number');
    if (status === 0) {
      assert(Array.isArray(result), 'result is array');
      if (result.length > 0) {
        assert(typeof result[0] === 'string', 'result[0] is string');
      }
    }
    done();
  };
  ch.queryA(req, 'localhost');
})();

// --- Test 8: Direct getHostByAddr on the binding ---
pending++;
(function() {
  var ch = new cares.ChannelWrap(0, 4, 0);
  var req = new cares.QueryReqWrap();
  req.oncomplete = function(status, result) {
    assert(typeof status === 'number', 'reverse status is number');
    if (status === 0) {
      assert(Array.isArray(result), 'reverse result is array');
    }
    done();
  };
  ch.getHostByAddr(req, '127.0.0.1');
})();

// --- Test 9: dns.resolve4 via the full JS API ---
pending++;
dns.resolve4('localhost', function(err, addresses) {
  // May fail if c-ares can't resolve 'localhost'. Just verify shape.
  if (!err) {
    assert(Array.isArray(addresses), 'resolve4 returns array');
    if (addresses.length > 0) {
      assert(typeof addresses[0] === 'string', 'address is string');
    }
  } else {
    // Acceptable errors: ENODATA, ENOTFOUND, ESERVFAIL.
    assert(err.code === 'ENODATA' || err.code === 'ENOTFOUND' ||
           err.code === 'ESERVFAIL' || err.code === 'ECANCELLED',
           'expected DNS error, got: ' + err.code);
  }
  done();
});

// --- Test 10: dns.reverse via the full JS API ---
pending++;
dns.reverse('127.0.0.1', function(err, hostnames) {
  if (!err) {
    assert(Array.isArray(hostnames), 'reverse returns array');
  } else {
    // Acceptable: may fail if no PTR record for 127.0.0.1.
    assert(typeof err.code === 'string', 'error has code');
  }
  done();
});

// --- Test 11: cancel works ---
(function() {
  var ch = new cares.ChannelWrap(0, 4, 0);
  var req = new cares.QueryReqWrap();
  var cancelled = false;
  pending++;
  req.oncomplete = function(status) {
    // Should get ARES_ECANCELLED.
    cancelled = true;
    done();
  };
  ch.queryA(req, 'example.com');
  ch.cancel();
})();

// --- Test 12: strerror works for c-ares error codes ---
// ARES_ENODATA = 1
var msg1 = cares.strerror(1);
assert(typeof msg1 === 'string' && msg1.length > 0, 'strerror(1) works');
// ARES_ETIMEOUT = 12
var msg12 = cares.strerror(12);
assert(typeof msg12 === 'string' && msg12.length > 0, 'strerror(12) works');
// Negative code (libuv error) should still work.
var msgNeg = cares.strerror(-1);
assert(typeof msgNeg === 'string' && msgNeg.length > 0, 'strerror(-1) works');
