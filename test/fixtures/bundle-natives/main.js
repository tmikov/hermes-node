// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

const addon = require('./hello_addon.node');
if (addon.hello() !== 'world') throw new Error('hello: ' + addon.hello());
if (addon.add(2, 3) !== 5) throw new Error('add: ' + addon.add(2, 3));
console.log('PASS');
