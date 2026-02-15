// Copyright (c) Tzvetan Mikov.
//
// Minimal AbortController/AbortSignal shim for Hermes.
// The original Node.js internal/abort_controller.js depends on:
//   - internal/event_target (which needs internalBinding('performance'))
//   - FinalizationRegistry (not available in Hermes)
//   - internal/webidl
//   - internal/worker/js_transferable
//
// This shim provides a minimal implementation sufficient for stream pipeline
// abort signaling without those heavy dependencies.

'use strict';

var EventEmitter = require('events');

function AbortSignal() {
  EventEmitter.call(this);
  this.aborted = false;
  this.reason = undefined;
}

Object.setPrototypeOf(AbortSignal.prototype, EventEmitter.prototype);
Object.setPrototypeOf(AbortSignal, EventEmitter);

Object.defineProperty(AbortSignal.prototype, 'onabort', {
  get: function() { return this._onabort || null; },
  set: function(v) { this._onabort = v; },
  enumerable: true,
  configurable: true,
});

AbortSignal.prototype.addEventListener = function(type, listener) {
  this.on(type, listener);
};

AbortSignal.prototype.removeEventListener = function(type, listener) {
  this.removeListener(type, listener);
};

AbortSignal.prototype.dispatchEvent = function(event) {
  this.emit(event.type, event);
  return true;
};

AbortSignal.prototype.throwIfAborted = function() {
  if (this.aborted) {
    throw this.reason;
  }
};

AbortSignal.abort = function(reason) {
  var signal = new AbortSignal();
  signal.aborted = true;
  signal.reason = reason !== undefined ? reason : new DOMException('This operation was aborted', 'AbortError');
  return signal;
};

AbortSignal.timeout = function(ms) {
  var signal = new AbortSignal();
  setTimeout(function() {
    if (!signal.aborted) {
      signal.aborted = true;
      signal.reason = new DOMException('The operation was aborted due to timeout', 'TimeoutError');
      signal.emit('abort');
      if (typeof signal._onabort === 'function') signal._onabort();
    }
  }, ms);
  return signal;
};

AbortSignal.any = function(signals) {
  var signal = new AbortSignal();
  for (var i = 0; i < signals.length; i++) {
    if (signals[i].aborted) {
      signal.aborted = true;
      signal.reason = signals[i].reason;
      return signal;
    }
  }
  function onAbort() {
    if (!signal.aborted) {
      signal.aborted = true;
      signal.reason = this.reason;
      signal.emit('abort');
      if (typeof signal._onabort === 'function') signal._onabort();
    }
  }
  for (var j = 0; j < signals.length; j++) {
    signals[j].addEventListener('abort', onAbort.bind(signals[j]));
  }
  return signal;
};

// DOMException polyfill (minimal)
function DOMException(message, name) {
  Error.call(this, message);
  this.message = message || '';
  this.name = name || 'Error';
  this.code = name === 'AbortError' ? 20 : 0;
}
Object.setPrototypeOf(DOMException.prototype, Error.prototype);

function AbortController() {
  this.signal = new AbortSignal();
}

AbortController.prototype.abort = function(reason) {
  var signal = this.signal;
  if (signal.aborted) return;
  signal.aborted = true;
  signal.reason = reason !== undefined ? reason : new DOMException('This operation was aborted', 'AbortError');
  signal.emit('abort');
  if (typeof signal._onabort === 'function') signal._onabort();
};

module.exports = {
  AbortController: AbortController,
  AbortSignal: AbortSignal,
};
