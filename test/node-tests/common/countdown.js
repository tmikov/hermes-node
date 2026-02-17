// Minimal shim of Node.js test/common/countdown.js for hermes-node.
'use strict';

var assert = require('assert');
var kLimit = Symbol('limit');
var kCallback = Symbol('callback');
var common = require('./');

function Countdown(limit, cb) {
  assert.strictEqual(typeof limit, 'number');
  assert.strictEqual(typeof cb, 'function');
  this[kLimit] = limit;
  this[kCallback] = common.mustCall(cb);
}

Countdown.prototype.dec = function() {
  assert(this[kLimit] > 0, 'Countdown expired');
  if (--this[kLimit] === 0)
    this[kCallback]();
  return this[kLimit];
};

Object.defineProperty(Countdown.prototype, 'remaining', {
  get: function() { return this[kLimit]; }
});

module.exports = Countdown;
