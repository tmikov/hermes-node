// Copyright (c) Tzvetan Mikov.
//
// Minimal domain shim for REPL support.
// The real domain.js depends on async_hooks and internal/async_hooks which
// need deep native binding support (async_context_frame). This shim provides
// just enough for the REPL's error isolation use case.

'use strict';

var EventEmitter = globalThis.require('events');
var ReflectApply = globalThis.primordials.ReflectApply;
var ArrayPrototypeSlice = globalThis.primordials.ArrayPrototypeSlice;
var ArrayPrototypePush = globalThis.primordials.ArrayPrototypePush;
var ArrayPrototypeLastIndexOf = globalThis.primordials.ArrayPrototypeLastIndexOf;
var ArrayPrototypeSplice = globalThis.primordials.ArrayPrototypeSplice;
var ObjectDefineProperty = globalThis.primordials.ObjectDefineProperty;

// The active domain stack.
var stack = [];
exports._stack = stack;

// Overwrite process.domain with a getter/setter.
var _domain = [null];
ObjectDefineProperty(process, 'domain', {
  __proto__: null,
  enumerable: true,
  get: function() {
    return _domain[0];
  },
  set: function(arg) {
    return _domain[0] = arg;
  },
});

// The active domain is always the one that we're currently in.
exports.active = null;

class Domain extends EventEmitter {
  constructor() {
    super();
    this.members = [];
  }
}

exports.Domain = Domain;

exports.create = exports.createDomain = function createDomain() {
  return new Domain();
};

Domain.prototype.members = undefined;

Domain.prototype.enter = function() {
  exports.active = process.domain = this;
  ArrayPrototypePush(stack, this);
};

Domain.prototype.exit = function() {
  var index = ArrayPrototypeLastIndexOf(stack, this);
  if (index === -1) return;

  ArrayPrototypeSplice(stack, index);

  exports.active = stack.length === 0 ? undefined : stack[stack.length - 1];
  process.domain = exports.active;
};

Domain.prototype.add = function(ee) {
  if (ee.domain === this)
    return;

  if (ee.domain)
    ee.domain.remove(ee);

  ObjectDefineProperty(ee, 'domain', {
    __proto__: null,
    configurable: true,
    enumerable: false,
    value: this,
    writable: true,
  });
  ArrayPrototypePush(this.members, ee);
};

Domain.prototype.remove = function(ee) {
  ee.domain = null;
  var index = this.members.indexOf(ee);
  if (index !== -1)
    ArrayPrototypeSplice(this.members, index, 1);
};

Domain.prototype.run = function(fn) {
  this.enter();
  var ret;
  try {
    ret = ReflectApply(fn, this, ArrayPrototypeSlice(arguments, 1));
  } finally {
    this.exit();
  }
  return ret;
};

Domain.prototype.bind = function(cb) {
  var self = this;

  function runBound() {
    self.enter();
    var ret;
    try {
      ret = ReflectApply(cb, this, arguments);
    } finally {
      self.exit();
    }
    return ret;
  }

  ObjectDefineProperty(runBound, 'domain', {
    __proto__: null,
    configurable: true,
    enumerable: false,
    value: this,
    writable: true,
  });

  return runBound;
};

Domain.prototype.intercept = function(cb) {
  var self = this;

  function runIntercepted() {
    var args = arguments;
    if (args[0] && args[0] instanceof Error) {
      var er = args[0];
      er.domainBound = cb;
      er.domainThrown = false;
      ObjectDefineProperty(er, 'domain', {
        __proto__: null,
        configurable: true,
        enumerable: false,
        value: self,
        writable: true,
      });
      self.emit('error', er);
      return;
    }

    self.enter();
    var ret;
    try {
      ret = ReflectApply(cb, this, ArrayPrototypeSlice(args, 1));
    } finally {
      self.exit();
    }
    return ret;
  }

  return runIntercepted;
};

Domain.prototype._errorHandler = function(er) {
  if ((typeof er === 'object' && er !== null) || typeof er === 'function') {
    ObjectDefineProperty(er, 'domain', {
      __proto__: null,
      configurable: true,
      enumerable: false,
      value: this,
      writable: true,
    });
    er.domainThrown = true;
  }

  // Pop all adjacent duplicates of the currently active domain.
  while (exports.active === this) {
    this.exit();
  }

  var caught = false;
  if (this.listenerCount('error') > 0) {
    try {
      caught = this.emit('error', er);
    } catch (er2) {
      // The domain error handler threw.
      throw er2;
    }
  }

  // Clear the stack.
  stack.length = 0;
  exports.active = process.domain = null;

  return caught;
};
