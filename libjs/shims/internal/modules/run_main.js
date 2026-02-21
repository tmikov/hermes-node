// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Shim for internal/modules/run_main.js
//
// The real module needs internal/process/execution and internalBinding('errors')
// for triggerUncaughtException. This shim provides executeUserEntryPoint which
// is only assigned to Module.runMain by initializeCJS(). Our bootstrap uses
// __loadUserScript() instead, so this is a stub.

'use strict';

function executeUserEntryPoint(main) {
  // Stub: our bootstrap uses __loadUserScript() instead of Module.runMain().
  throw new Error('executeUserEntryPoint is not implemented; use __loadUserScript');
}

module.exports = {
  executeUserEntryPoint,
};
