/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Minimal shim for internal/vm/module.
// The real module (552 lines) depends on internalBinding('module_wrap') and
// other deep VM internals we don't have. Only importModuleDynamicallyWrap is
// needed by the REPL (via internal/vm.js registerImportModuleDynamically).
// Since we don't support ESM dynamic imports, we just pass the callback
// through unchanged.

'use strict';

function importModuleDynamicallyWrap(importModuleDynamically) {
  return importModuleDynamically;
}

module.exports = {
  importModuleDynamicallyWrap,
};
