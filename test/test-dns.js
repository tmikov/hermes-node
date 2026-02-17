// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the dns module — dns.lookup() via cares_wrap binding.

'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

// --- Test 1: cares_wrap binding basics ---
var cares = internalBinding('cares_wrap');
assert(typeof cares.getaddrinfo === 'function', 'getaddrinfo should be a function');
assert(typeof cares.getnameinfo === 'function', 'getnameinfo should be a function');
assert(typeof cares.strerror === 'function', 'strerror should be a function');
assert(typeof cares.canonicalizeIP === 'function', 'canonicalizeIP should be a function');
assert(typeof cares.GetAddrInfoReqWrap === 'function', 'GetAddrInfoReqWrap should be a constructor');
assert(typeof cares.GetNameInfoReqWrap === 'function', 'GetNameInfoReqWrap should be a constructor');
assert(typeof cares.QueryReqWrap === 'function', 'QueryReqWrap should be a constructor');
assert(typeof cares.ChannelWrap === 'function', 'ChannelWrap should be a constructor');

// Constants.
assert(typeof cares.AF_INET === 'number', 'AF_INET');
assert(typeof cares.AF_INET6 === 'number', 'AF_INET6');
assert(typeof cares.AF_UNSPEC === 'number', 'AF_UNSPEC');
assert(typeof cares.AI_ADDRCONFIG === 'number', 'AI_ADDRCONFIG');
assert(typeof cares.AI_ALL === 'number', 'AI_ALL');
assert(typeof cares.AI_V4MAPPED === 'number', 'AI_V4MAPPED');
assert(cares.DNS_ORDER_VERBATIM === 0, 'DNS_ORDER_VERBATIM');
assert(cares.DNS_ORDER_IPV4_FIRST === 1, 'DNS_ORDER_IPV4_FIRST');
assert(cares.DNS_ORDER_IPV6_FIRST === 2, 'DNS_ORDER_IPV6_FIRST');

// --- Test 2: canonicalizeIP ---
assert(cares.canonicalizeIP('127.0.0.1') === '127.0.0.1', 'canonicalize IPv4');
assert(cares.canonicalizeIP('::1') === '::1', 'canonicalize IPv6 loopback');
assert(cares.canonicalizeIP('0:0:0:0:0:0:0:1') === '::1', 'canonicalize IPv6 expanded');
assert(cares.canonicalizeIP('not-an-ip') === undefined, 'canonicalize invalid');

// --- Test 3: strerror ---
var errMsg = cares.strerror(-1);
assert(typeof errMsg === 'string' && errMsg.length > 0, 'strerror returns string');

// --- Test 4: GetAddrInfoReqWrap constructor ---
var reqObj = new cares.GetAddrInfoReqWrap();
assert(typeof reqObj === 'object', 'GetAddrInfoReqWrap returns object');

// --- Test 5: dns module loads ---
var dns = require('dns');
assert(typeof dns.lookup === 'function', 'dns.lookup is a function');
assert(typeof dns.ADDRCONFIG === 'number', 'dns.ADDRCONFIG');
assert(typeof dns.ALL === 'number', 'dns.ALL');
assert(typeof dns.V4MAPPED === 'number', 'dns.V4MAPPED');

// Track completion of async tests.
var pending = 0;
function done() {
  pending--;
  if (pending === 0) {
    console.log('PASS');
  }
}

// --- Test 6: dns.lookup('localhost') ---
pending++;
dns.lookup('localhost', function(err, address, family) {
  assert(!err, 'dns.lookup(localhost) failed: ' + (err ? err.message : ''));
  assert(typeof address === 'string', 'address is string: ' + address);
  assert(family === 4 || family === 6, 'family is 4 or 6: ' + family);
  done();
});

// --- Test 7: dns.lookup with IP passthrough (no network) ---
pending++;
dns.lookup('127.0.0.1', function(err, address, family) {
  assert(!err, 'dns.lookup(127.0.0.1) failed: ' + (err ? err.message : ''));
  assert(address === '127.0.0.1', 'address should be 127.0.0.1: ' + address);
  assert(family === 4, 'family should be 4: ' + family);
  done();
});

// --- Test 8: dns.lookup with IPv6 passthrough ---
pending++;
dns.lookup('::1', function(err, address, family) {
  assert(!err, 'dns.lookup(::1) failed: ' + (err ? err.message : ''));
  assert(address === '::1', 'address should be ::1: ' + address);
  assert(family === 6, 'family should be 6: ' + family);
  done();
});

// --- Test 9: dns.lookup with all:true ---
pending++;
dns.lookup('localhost', { all: true }, function(err, addresses) {
  assert(!err, 'dns.lookup(localhost, all) failed: ' + (err ? err.message : ''));
  assert(Array.isArray(addresses), 'addresses should be array');
  assert(addresses.length > 0, 'should have at least one address');
  assert(typeof addresses[0].address === 'string', 'address[0].address is string');
  assert(addresses[0].family === 4 || addresses[0].family === 6, 'family is 4 or 6');
  done();
});

// --- Test 10: dns.lookup with family:4 ---
pending++;
dns.lookup('localhost', { family: 4 }, function(err, address, family) {
  // This may fail on systems without IPv4 localhost, so just check the shape.
  if (!err) {
    assert(typeof address === 'string', 'address is string');
    assert(family === 4, 'family should be 4');
  }
  done();
});

// --- Test 11: dns.lookup with empty hostname ---
pending++;
dns.lookup('', function(err, address, family) {
  // Empty hostname should return null address.
  assert(!err, 'empty hostname should not error');
  assert(address === null, 'address should be null for empty hostname');
  done();
});

// --- Test 12: ChannelWrap stub works ---
var channel = new cares.ChannelWrap(0, 4, 0);
assert(typeof channel.getServers === 'function', 'ChannelWrap.getServers');
assert(typeof channel.setServers === 'function', 'ChannelWrap.setServers');
assert(typeof channel.cancel === 'function', 'ChannelWrap.cancel');
var servers = channel.getServers();
assert(Array.isArray(servers), 'getServers returns array');
