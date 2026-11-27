// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Run by test/lit.cfg to decide the `ui-imgui` feature, and by
// test/ui-imgui-feature.js to check that decision: prints `imgui ok
// <version>` when `hermes-node:imgui` loads, and throws otherwise.
console.log('imgui ok ' + require('hermes-node:imgui').version);
