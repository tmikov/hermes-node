// Copyright (c) Tzvetan Mikov.
//
// Loads and installs the real console module, replacing the minimal C++ console.
// Called during hermes-node bootstrap after stdio streams are set up.

'use strict';

(function initConsole() {
  var c = require('console');
  var ctor = require('internal/console/constructor');
  c[ctor.kBindStreamsLazy](process);
  globalThis.console = c;
})();
