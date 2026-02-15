// Copyright (c) Tzvetan Mikov.
//
// Minimal internal/event_target shim for Hermes.
// The original depends on internalBinding('performance') via internal/perf/utils
// and other heavy dependencies. This shim provides the symbols and stubs needed
// by consumers like internal/streams/operators.js, events.js, and fs.js.

'use strict';

const {
  Symbol,
  SymbolFor,
} = primordials;

const kWeakHandler = Symbol('kWeak');
const kResistStopPropagation = Symbol('kResistStopPropagation');
const kCreateEvent = Symbol('kCreateEvent');
const kNewListener = Symbol('kNewListener');
const kTrustEvent = Symbol('kTrustEvent');
const kRemoveListener = Symbol('kRemoveListener');
const kEvents = Symbol('kEvents');
const kIsEventTarget = SymbolFor('nodejs.event_target');

// Minimal Event class (DOM-style).
class Event {
  constructor(type, options) {
    this.type = type;
    this.defaultPrevented = false;
    this.cancelable = !!(options && options.cancelable);
    this.timeStamp = Date.now();
  }
  preventDefault() {
    if (this.cancelable) this.defaultPrevented = true;
  }
  stopImmediatePropagation() {}
  stopPropagation() {}
}

// Minimal CustomEvent class.
class CustomEvent extends Event {
  constructor(type, options) {
    super(type, options);
    this.detail = options && options.detail !== undefined ? options.detail : null;
  }
}

// Stub EventTarget -- not functional, but enough to satisfy destructuring.
class EventTarget {
  static [kIsEventTarget] = true;
  addEventListener() {}
  removeEventListener() {}
  dispatchEvent() { return true; }
}

class NodeEventTarget extends EventTarget {}

function defineEventHandler(emitter, name) {}
function initEventTarget(self) {}
function initNodeEventTarget(self) {}
function isEventTarget(obj) {
  return obj?.constructor?.[kIsEventTarget] ?? false;
}

module.exports = {
  Event,
  CustomEvent,
  EventTarget,
  NodeEventTarget,
  defineEventHandler,
  initEventTarget,
  initNodeEventTarget,
  kCreateEvent,
  kNewListener,
  kTrustEvent,
  kRemoveListener,
  kEvents,
  kWeakHandler,
  kResistStopPropagation,
  isEventTarget,
};
