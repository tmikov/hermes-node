// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Set up a node_modules/hello-addon/ package with the built .node file,
// then run main.js which does require('hello-addon').
// RUN: mkdir -p %t/node_modules/hello-addon
// RUN: cp %hello_addon %t/node_modules/hello-addon/hello_addon.node
// RUN: echo '{"name":"hello-addon","main":"hello_addon.node"}' > %t/node_modules/hello-addon/package.json
// RUN: cp %source_dir/test/fixtures/native-addon-pkg/main.js %t/main.js
// RUN: %hermes-node %t/main.js | %FileCheck %s
// CHECK: PASS
