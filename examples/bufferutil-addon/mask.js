// Demonstrates loading a real npm native NAPI addon (bufferutil).
// bufferutil provides fast WebSocket frame masking/unmasking in C.
//
// Install: npm install
// Run:     cmake-build-asan/bin/hermes-node examples/bufferutil-addon/mask.js

'use strict';

const bufferutil = require('bufferutil');

console.log('bufferutil loaded, exports:', Object.keys(bufferutil));

// Verify the native .node addon was loaded, not the pure-JS fallback.
const path = require('path');
const resolve = require('node-gyp-build').path;
const addonPath = resolve(path.dirname(require.resolve('bufferutil')));
if (!addonPath.endsWith('.node'))
  throw new Error('Expected native addon, got: ' + addonPath);
console.log('native addon:', addonPath);

// WebSocket frames are XOR-masked with a 4-byte key.
const data = Buffer.from('Hello, World!');
const mask = Buffer.from([0x12, 0x34, 0x56, 0x78]);
const output = Buffer.alloc(data.length);
data.copy(output);

bufferutil.mask(output, mask, output, 0, output.length);
console.log('masked:  ', output.toString('hex'));

bufferutil.unmask(output, mask);
console.log('unmasked:', output.toString());

console.log('PASS');
