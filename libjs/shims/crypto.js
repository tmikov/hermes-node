// Copyright (c) Tzvetan Mikov.
// Minimal crypto shim backed by picohash (MD5, SHA1, SHA224, SHA256, HMAC).

'use strict';

var {
  Hash: _Hash,
  Hmac: _Hmac,
  getHashes,
  timingSafeEqual,
  randomFillSync: _randomFillSync,
} = internalBinding('crypto');

var { Transform } = require('stream');
var { Buffer } = require('buffer');

// Hash class (extends Transform for .pipe() compatibility).
function Hash(algorithm, options) {
  if (!(this instanceof Hash))
    return new Hash(algorithm, options);
  Transform.call(this, options);
  this._hash = new _Hash(algorithm);
  this._finalized = false;
}

Object.setPrototypeOf(Hash.prototype, Transform.prototype);
Object.setPrototypeOf(Hash, Transform);

Hash.prototype.update = function update(data, inputEncoding) {
  if (this._finalized)
    throw new Error('Digest already called');
  if (typeof data === 'string')
    data = Buffer.from(data, inputEncoding || 'utf8');
  this._hash.update(data);
  return this;
};

Hash.prototype.digest = function digest(outputEncoding) {
  if (this._finalized)
    throw new Error('Digest already called');
  this._finalized = true;
  var buf = Buffer.from(this._hash.digest());
  if (outputEncoding && outputEncoding !== 'buffer')
    return buf.toString(outputEncoding);
  return buf;
};

Hash.prototype.copy = function copy(options) {
  if (this._finalized)
    throw new Error('Digest already called');
  var h = new Hash('md5', options); // dummy algo, overwritten by native copy
  h._hash = this._hash.copy();
  h._finalized = false;
  return h;
};

Hash.prototype._transform = function _transform(chunk, encoding, callback) {
  this.update(chunk, encoding);
  callback();
};

Hash.prototype._flush = function _flush(callback) {
  this.push(this.digest());
  callback();
};

// Hmac class (extends Transform for .pipe() compatibility).
function Hmac(algorithm, key, options) {
  if (!(this instanceof Hmac))
    return new Hmac(algorithm, key, options);
  Transform.call(this, options);
  if (typeof key === 'string')
    key = Buffer.from(key, 'utf8');
  this._hmac = new _Hmac();
  this._hmac.init(algorithm, key);
  this._finalized = false;
}

Object.setPrototypeOf(Hmac.prototype, Transform.prototype);
Object.setPrototypeOf(Hmac, Transform);

Hmac.prototype.update = function update(data, inputEncoding) {
  if (this._finalized)
    throw new Error('Digest already called');
  if (typeof data === 'string')
    data = Buffer.from(data, inputEncoding || 'utf8');
  this._hmac.update(data);
  return this;
};

Hmac.prototype.digest = function digest(outputEncoding) {
  if (this._finalized)
    throw new Error('Digest already called');
  this._finalized = true;
  var buf = Buffer.from(this._hmac.digest());
  if (outputEncoding && outputEncoding !== 'buffer')
    return buf.toString(outputEncoding);
  return buf;
};

Hmac.prototype._transform = function _transform(chunk, encoding, callback) {
  this.update(chunk, encoding);
  callback();
};

Hmac.prototype._flush = function _flush(callback) {
  this.push(this.digest());
  callback();
};

// One-shot hash function.
function hash(algorithm, data, outputEncoding) {
  if (typeof data === 'string')
    data = Buffer.from(data, 'utf8');
  var h = new Hash(algorithm);
  h.update(data);
  return h.digest(outputEncoding || 'hex');
}

function createHash(algorithm, options) {
  return new Hash(algorithm, options);
}

function createHmac(algorithm, key, options) {
  return new Hmac(algorithm, key, options);
}

function randomFillSync(buf, offset, size) {
  if (typeof buf !== 'object' || buf === null)
    throw new TypeError('buf must be a Buffer, TypedArray, or DataView');
  var byteLength = buf.byteLength;
  if (offset === undefined) offset = 0;
  if (size === undefined) size = byteLength - offset;
  if (offset < 0 || offset > byteLength)
    throw new RangeError('offset is out of range');
  if (size < 0 || offset + size > byteLength)
    throw new RangeError('size is out of range');
  if (size === 0) return buf;
  // Native expects a TypedArray/Buffer.
  var target = buf;
  if (buf instanceof ArrayBuffer)
    target = new Uint8Array(buf);
  else if (ArrayBuffer.isView(buf) && !(buf instanceof Uint8Array))
    target = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
  _randomFillSync(target, offset, size);
  return buf;
}

function randomBytes(size, callback) {
  if (typeof size !== 'number' || size < 0 || size !== (size >>> 0))
    throw new RangeError('size must be a non-negative uint32');
  var buf = Buffer.alloc(size);
  if (callback === undefined) {
    randomFillSync(buf, 0, size);
    return buf;
  }
  if (typeof callback !== 'function')
    throw new TypeError('callback must be a function');
  try {
    randomFillSync(buf, 0, size);
    process.nextTick(callback, null, buf);
  } catch (e) {
    process.nextTick(callback, e);
  }
}

function randomInt(min, max, callback) {
  // Supports randomInt(max), randomInt(min, max), randomInt(min, max, cb).
  if (max === undefined || typeof max === 'function') {
    callback = max;
    max = min;
    min = 0;
  }
  if (!Number.isSafeInteger(min)) throw new TypeError('min must be a safe integer');
  if (!Number.isSafeInteger(max)) throw new TypeError('max must be a safe integer');
  if (max <= min) throw new RangeError('max must be greater than min');
  var range = max - min;
  if (range > 0xFFFFFFFFFFFF)
    throw new RangeError('max - min must be <= 2^48 - 1');
  // Use rejection sampling for uniform distribution.
  var buf = Buffer.alloc(6);
  var RAND_MAX = 0xFFFFFFFFFFFF;
  var randLimit = RAND_MAX - (RAND_MAX % range);
  while (true) {
    randomFillSync(buf);
    var x = buf.readUIntBE(0, 6);
    if (x < randLimit) {
      var n = (x % range) + min;
      if (callback) { process.nextTick(callback, null, n); return; }
      return n;
    }
  }
}

module.exports = {
  Hash,
  Hmac,
  createHash,
  createHmac,
  getHashes,
  timingSafeEqual,
  hash,
  randomBytes,
  randomFillSync,
  randomInt,
  constants: {},
};
