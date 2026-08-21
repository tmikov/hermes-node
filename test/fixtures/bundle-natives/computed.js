// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

const name = './hello_addon' + '.node';
const addon = require(name);
if (addon.hello() !== 'world') throw new Error('hello: ' + addon.hello());
console.log('PASS');
