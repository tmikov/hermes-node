// Copyright (c) Tzvetan Mikov.
//
// Sets up process event emitter methods (on/off/once/emit/emitWarning etc.).
// Called during hermes-node bootstrap after process object is on globalThis.

'use strict';

(function setupProcessEvents() {
  var process = globalThis.process;
  var handlers = {};
  process.on = process.addListener = function(event, fn) {
    if (!handlers[event]) handlers[event] = [];
    if (event !== 'newListener') process.emit('newListener', event, fn);
    handlers[event].push(fn);
    return process;
  };
  process.prependListener = function(event, fn) {
    if (!handlers[event]) handlers[event] = [];
    if (event !== 'newListener') process.emit('newListener', event, fn);
    handlers[event].unshift(fn);
    return process;
  };
  process.prependOnceListener = function(event, fn) {
    function wrapper() {
      process.off(event, wrapper);
      fn.apply(this, arguments);
    }
    return process.prependListener(event, wrapper);
  };
  process.off = process.removeListener = function(event, fn) {
    var list = handlers[event];
    if (list) {
      var idx = list.indexOf(fn);
      if (idx >= 0) list.splice(idx, 1);
    }
    return process;
  };
  process.once = function(event, fn) {
    function wrapper() {
      process.off(event, wrapper);
      fn.apply(this, arguments);
    }
    return process.on(event, wrapper);
  };
  process.emit = function(event) {
    var list = handlers[event];
    if (!list) return false;
    var args = Array.prototype.slice.call(arguments, 1);
    var copy = list.slice();
    for (var i = 0; i < copy.length; i++) {
      copy[i].apply(process, args);
    }
    return true;
  };
  process.listeners = function(event) {
    return (handlers[event] || []).slice();
  };
  process.listenerCount = function(event) {
    return (handlers[event] || []).length;
  };
  process.rawListeners = function(event) {
    return (handlers[event] || []).slice();
  };
  process.removeAllListeners = function(event) {
    if (event !== undefined) { delete handlers[event]; }
    else { handlers = {}; }
    return process;
  };
  process.emitWarning = function(warning, type, code) {
    if (typeof type === 'object' && type !== null) {
      code = type.code; type = type.type || type.name;
    }
    if (typeof warning === 'string') {
      var w = new Error(warning);
      w.name = type || 'Warning';
      if (code) w.code = code;
      warning = w;
    }
    process.emit('warning', warning);
  };
})();
