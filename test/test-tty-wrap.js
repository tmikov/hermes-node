// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test for the tty_wrap binding.
// Note: require('tty') depends on net.js which needs cares_wrap/tcp_wrap/pipe_wrap,
// so we only test the native binding directly here.
'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var ttyWrap = internalBinding('tty_wrap');

// --- Exports ---
assert(typeof ttyWrap.TTY === 'function', 'TTY is a function');
assert(typeof ttyWrap.isTTY === 'function', 'isTTY is a function');

// --- isTTY ---
// isTTY should return a boolean for valid fds.
var result0 = ttyWrap.isTTY(0);
assert(typeof result0 === 'boolean', 'isTTY(0) returns boolean');
var result1 = ttyWrap.isTTY(1);
assert(typeof result1 === 'boolean', 'isTTY(1) returns boolean');
var result2 = ttyWrap.isTTY(2);
assert(typeof result2 === 'boolean', 'isTTY(2) returns boolean');
// A very high fd should not be a TTY.
var resultHigh = ttyWrap.isTTY(9999);
assert(resultHigh === false, 'isTTY(9999) is false');

// --- TTY constructor ---
// Test constructing a TTY on fd 1. The behavior depends on whether it's a
// real TTY or a pipe (CI redirected output).
var ctx = {};
var handle = new ttyWrap.TTY(1, ctx);
if (ctx.code === undefined) {
  // Construction succeeded — verify methods exist and work.

  // getWindowSize
  assert(typeof handle.getWindowSize === 'function', 'getWindowSize exists');
  var winSize = [0, 0];
  var err = handle.getWindowSize(winSize);
  assert(typeof err === 'number', 'getWindowSize returns a number');

  // If fd 1 is a real TTY, window size should be valid.
  if (ttyWrap.isTTY(1) && err === 0) {
    assert(winSize[0] > 0, 'width > 0: ' + winSize[0]);
    assert(winSize[1] > 0, 'height > 0: ' + winSize[1]);
  }

  // setRawMode
  assert(typeof handle.setRawMode === 'function', 'setRawMode exists');

  // Stream methods from LibuvStreamBase
  assert(typeof handle.readStart === 'function', 'readStart exists');
  assert(typeof handle.readStop === 'function', 'readStop exists');
  assert(typeof handle.writeBuffer === 'function', 'writeBuffer exists');
  assert(typeof handle.writeUtf8String === 'function', 'writeUtf8String exists');
  assert(typeof handle.shutdown === 'function', 'shutdown exists');
  assert(typeof handle.setBlocking === 'function', 'setBlocking exists');

  // Handle methods
  assert(typeof handle.ref === 'function', 'ref exists');
  assert(typeof handle.unref === 'function', 'unref exists');
  assert(typeof handle.hasRef === 'function', 'hasRef exists');
  assert(typeof handle.close === 'function', 'close exists');

  handle.close();
} else {
  // Construction failed — verify error context fields.
  assert(typeof ctx.errno === 'number', 'ctx.errno is a number');
  assert(typeof ctx.message === 'string', 'ctx.message is a string');
  assert(ctx.syscall === 'uv_tty_init', 'ctx.syscall is uv_tty_init');
}

console.log('PASS');
