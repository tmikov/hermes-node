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
  // Called from C++ with an exception that escaped an asynchronous callback
  // -- a timer, an immediate, a tick. The answer decides what happens next:
  // true means a listener took responsibility and the program carries on,
  // false means the caller reports the error and exits 1. Both the contract
  // and the division of labour are Node's (node::errors::TriggerUncaughtException
  // asks the same question of the same property), and the reason for the
  // split is that only JavaScript knows which listeners exist.
  //
  // Node assembles this in internal/process/execution.js and installs it
  // from internal/bootstrap/node.js, neither of which this runtime runs.
  // Reproduced here is what this runtime can support; there is no
  // setUncaughtExceptionCaptureCallback, so that branch of Node's version
  // has no equivalent yet.
  process._fatalException = function(er, fromPromise) {
    var type = fromPromise ? 'unhandledRejection' : 'uncaughtException';
    process.emit('uncaughtExceptionMonitor', er, type);
    if (!process.emit('uncaughtException', er, type)) {
      // Nobody took it. The 'exit' handlers run here rather than in the
      // bootstrap's usual place: the caller terminates as soon as this
      // returns, so step 15 -- which is what normally emits 'exit' and
      // settles the status -- is never reached.
      try {
        if (!process._exiting) {
          process._exiting = true;
          process.exitCode = 1;
          process.emit('exit', 1);
        }
      } catch (e) {
        // Already failing; a handler that throws on the way out cannot be
        // allowed to replace the error the program is actually about.
      }
      return false;
    }
    return true;
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
