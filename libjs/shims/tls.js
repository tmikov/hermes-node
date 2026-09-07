// Client TLS. OpenSSL filter over net.Socket. Enough for https.get.
'use strict';

const net = require('net');
const stream = require('stream');
const { Buffer } = require('buffer');
const { TLSWrap } = internalBinding('tls_wrap');

function TLSSocket(options, cb) {
  if (!(this instanceof TLSSocket))
    return new TLSSocket(options, cb);
  stream.Duplex.call(this);
  options = options || {};

  const host = options.servername || options.host || options.hostname || 'localhost';
  const port = options.port || 443;
  const self = this;
  this.connecting = true;
  this.authorized = false;
  this.encrypted = true;
  this._hadError = false;
  this.destroyed = false;
  this._pendingWrites = [];

  const wrap = new TLSWrap(host);
  this._tlsWrap = wrap;

  wrap.onencrypted = function (buf) {
    if (!self._socket || self._socket.destroyed)
      return;
    self._socket.write(buf);
  };
  wrap.onplaintext = function (buf) {
    self.push(buf);
  };
  wrap.onsecureconnect = function () {
    self.connecting = false;
    self.authorized = true;
    const pending = self._pendingWrites;
    self._pendingWrites = [];
    for (let i = 0; i < pending.length; i++) {
      const item = pending[i];
      self._write(item[0], item[1], item[2]);
    }
    self.emit('secureConnect');
    self.emit('connect');
    if (typeof cb === 'function')
      cb();
  };
  wrap.onerror = function (msg) {
    const err = new Error(msg);
    err.code = 'ERR_TLS_HANDSHAKE';
    self._hadError = true;
    self.emit('error', err);
    self.destroy();
  };
  wrap.onclose = function () {
    self.push(null);
  };

  const socket = options.socket || net.connect({
    host: options.host || options.hostname || host,
    port: port,
    family: options.family,
  });
  this._socket = socket;

  socket.on('data', function (chunk) {
    wrap.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  });
  socket.on('error', function (err) {
    self.emit('error', err);
  });
  socket.on('close', function () {
    if (!self.destroyed)
      self.push(null);
    self.emit('close');
  });
  socket.on('end', function () {
    self.push(null);
  });

  const start = function () {
    wrap.start();
  };
  if (socket.connecting)
    socket.once('connect', start);
  else
    start();
}

Object.setPrototypeOf(TLSSocket.prototype, stream.Duplex.prototype);
Object.setPrototypeOf(TLSSocket, stream.Duplex);

TLSSocket.prototype._read = function () {};

TLSSocket.prototype._write = function (chunk, encoding, callback) {
  if (!this.authorized) {
    this._pendingWrites.push([chunk, encoding, callback]);
    return;
  }
  try {
    this._tlsWrap.write(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk, encoding));
    callback();
  } catch (err) {
    callback(err);
  }
};

TLSSocket.prototype.setTimeout = function (msecs, cb) {
  if (this._socket && this._socket.setTimeout)
    this._socket.setTimeout(msecs, cb);
  return this;
};

TLSSocket.prototype.setNoDelay = function (noDelay) {
  if (this._socket && this._socket.setNoDelay)
    this._socket.setNoDelay(noDelay);
  return this;
};

TLSSocket.prototype.setKeepAlive = function (enable, initialDelay) {
  if (this._socket && this._socket.setKeepAlive)
    this._socket.setKeepAlive(enable, initialDelay);
  return this;
};

TLSSocket.prototype.ref = function () {
  if (this._socket && this._socket.ref)
    this._socket.ref();
  return this;
};

TLSSocket.prototype.unref = function () {
  if (this._socket && this._socket.unref)
    this._socket.unref();
  return this;
};

TLSSocket.prototype.destroy = function (err) {
  if (this.destroyed)
    return this;
  this.destroyed = true;
  try {
    if (this._tlsWrap)
      this._tlsWrap.shutdown();
  } catch (_) {}
  if (this._socket)
    this._socket.destroy(err);
  if (err)
    this.emit('error', err);
  this.emit('close');
  return this;
};

TLSSocket.prototype.end = function (data, encoding, cb) {
  if (data)
    this.write(data, encoding);
  stream.Duplex.prototype.end.call(this, undefined, undefined, cb);
  if (this._socket)
    this._socket.end();
  return this;
};

function connect(options, cb) {
  if (typeof options === 'number')
    options = { port: options };
  if (typeof options === 'string')
    options = { host: options };
  if (typeof cb !== 'function' && typeof arguments[1] === 'function')
    cb = arguments[1];
  if (typeof arguments[2] === 'function')
    cb = arguments[2];
  return new TLSSocket(options, cb);
}

function notImplemented() {
  throw new Error('tls server APIs are not implemented');
}

module.exports = {
  connect: connect,
  TLSSocket: TLSSocket,
  createSecureContext: function () { return {}; },
  createSecurePair: notImplemented,
  createServer: notImplemented,
  getCiphers: function () { return []; },
  DEFAULT_ECDH_CURVE: 'auto',
  DEFAULT_MAX_VERSION: 'TLSv1.3',
  DEFAULT_MIN_VERSION: 'TLSv1.2',
  SecureContext: function () {},
  Server: notImplemented,
};
