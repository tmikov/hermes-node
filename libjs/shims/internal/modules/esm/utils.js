// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Shim for internal/modules/esm/utils.
// The real module depends on internalBinding('module_wrap') with ModuleWrap,
// setImportModuleDynamicallyCallback, etc. Since we don't support ESM, this
// shim provides the subset needed by the CJS loader and other consumers.

'use strict';

var {
  ObjectFreeze,
  SafeSet,
} = primordials;

var { getOptionValue } = require('internal/options');

// Default conditions for module resolution.
var defaultConditions;
var defaultConditionsSet;

function initializeDefaultConditions() {
  var userConditions = getOptionValue('--conditions');
  var noAddons = getOptionValue('--no-addons');
  var addonConditions = noAddons ? [] : ['node-addons'];
  defaultConditions = ObjectFreeze([
    'node',
    'import',
    ...addonConditions,
    ...userConditions,
  ]);
  defaultConditionsSet = new SafeSet(defaultConditions);
}

function getDefaultConditions() {
  if (defaultConditions === undefined) {
    initializeDefaultConditions();
  }
  return defaultConditions;
}

function getDefaultConditionsSet() {
  if (defaultConditionsSet === undefined) {
    initializeDefaultConditions();
  }
  return defaultConditionsSet;
}

// getConditionsSet is used by the ESM resolver for package.json "exports"
// resolution. Returns the default conditions set.
function getConditionsSet() {
  return getDefaultConditionsSet();
}

function registerModule(referrer, registry) {
  // No-op: ESM not supported.
}

// requestTypes constants used by ESM translators.
var requestTypes = ObjectFreeze({
  kRequireInImportedCJS: 0,
});

function compileSourceTextModule() {
  throw new Error('ESM is not supported');
}

function initializeESM() {
  // No-op: ESM not supported.
}

module.exports = {
  compileSourceTextModule,
  getConditionsSet,
  getDefaultConditions,
  getDefaultConditionsSet,
  initializeDefaultConditions,
  initializeESM,
  registerModule,
  requestTypes,
};
