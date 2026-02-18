// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s

'use strict';

const formats = require('internal/modules/esm/formats');

// Check extensionFormatMap has expected entries
console.log(formats.extensionFormatMap['.cjs']);
// CHECK: commonjs
console.log(formats.extensionFormatMap['.js']);
// CHECK: module
console.log(formats.extensionFormatMap['.json']);
// CHECK: json
console.log(formats.extensionFormatMap['.mjs']);
// CHECK: module
console.log(formats.extensionFormatMap['.wasm']);
// CHECK: wasm

// Check mimeToFormat
console.log(formats.mimeToFormat('application/javascript'));
// CHECK: module
console.log(formats.mimeToFormat('text/javascript'));
// CHECK: module
console.log(formats.mimeToFormat('text/javascript; charset=utf-8'));
// CHECK: module
console.log(formats.mimeToFormat('application/json'));
// CHECK: json
console.log(formats.mimeToFormat('application/wasm'));
// CHECK: wasm
console.log(formats.mimeToFormat('text/plain'));
// CHECK: null

// Check getFormatOfExtensionlessFile returns a string
console.log(typeof formats.getFormatOfExtensionlessFile);
// CHECK: function

console.log('PASS');
// CHECK: PASS
