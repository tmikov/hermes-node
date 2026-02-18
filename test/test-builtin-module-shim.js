// Copyright (c) Tzvetan Mikov.
// Test the BuiltinModule shim from internal/bootstrap/realm.
// RUN: %hermes-node %s | %FileCheck %s
'use strict';

var { BuiltinModule } = require('internal/bootstrap/realm');

// getSchemeOnlyModuleNames returns an empty array.
var schemeOnly = BuiltinModule.getSchemeOnlyModuleNames();
console.log(Array.isArray(schemeOnly) && schemeOnly.length === 0);
// CHECK: true

// exists works for known modules.
console.log(BuiltinModule.exists('fs'));
// CHECK: true
console.log(BuiltinModule.exists('nonexistent'));
// CHECK: false

// canBeRequiredByUsers
console.log(BuiltinModule.canBeRequiredByUsers('path'));
// CHECK: true
console.log(BuiltinModule.canBeRequiredByUsers('_http_agent'));
// CHECK: false

// canBeRequiredWithoutScheme
console.log(BuiltinModule.canBeRequiredWithoutScheme('events'));
// CHECK: true

// isBuiltin handles node: prefix
console.log(BuiltinModule.isBuiltin('fs'));
// CHECK: true
console.log(BuiltinModule.isBuiltin('node:fs'));
// CHECK: true
console.log(BuiltinModule.isBuiltin('node:nonexistent'));
// CHECK: false

// normalizeRequirableId
console.log(BuiltinModule.normalizeRequirableId('node:fs'));
// CHECK: fs
console.log(BuiltinModule.normalizeRequirableId('fs'));
// CHECK: fs
console.log(BuiltinModule.normalizeRequirableId('node:nonexistent'));
// CHECK: undefined
console.log(BuiltinModule.normalizeRequirableId('nonexistent'));
// CHECK: undefined

// getAllBuiltinModuleIds returns a non-empty array
var all = BuiltinModule.getAllBuiltinModuleIds();
console.log(Array.isArray(all) && all.length > 0 && all.includes('fs'));
// CHECK: true

// map is populated
console.log(BuiltinModule.map.has('fs'));
// CHECK: true
console.log(BuiltinModule.map.get('fs').id);
// CHECK: fs

console.log('PASS');
// CHECK: PASS
