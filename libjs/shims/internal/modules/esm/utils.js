/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Minimal shim for internal/modules/esm/utils.
// The real module depends on internalBinding('module_wrap') and the full ESM
// loader infrastructure. Only registerModule is needed by internal/vm.js for
// the REPL's importModuleDynamically support. Since we don't support ESM,
// registerModule is a no-op.

'use strict';

function registerModule(referrer, registry) {
  // No-op: ESM not supported.
}

module.exports = {
  registerModule,
};
