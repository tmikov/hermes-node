// Copyright (c) Tzvetan Mikov.
// Minimal crypto shim backed by picohash (MD5, SHA1, SHA224, SHA256, HMAC).

'use strict';

var {
  Hash: _Hash,
  Hmac: _Hmac,
  getHashes,
  timingSafeEqual,
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

module.exports = {
  Hash,
  Hmac,
  createHash,
  createHmac,
  getHashes,
  timingSafeEqual,
  hash,
  constants: {},
};
