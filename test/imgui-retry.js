// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A failed `require('hermes-node:imgui')` is retried, through to success,
// in one process: test/imgui-missing.js's script, given the real addon's
// path as its third argument, so that after the absent-file and
// unloadable-file attempts it puts the real addon in place and requires it
// once more. Kept apart from imgui-missing.js because this half needs a
// built addon (`ui-imgui`), and that one's coverage should survive without
// it. Its header lists the mutations.
//
// The addon is linked or copied into %t.dir by the script itself, after
// the earlier attempts have failed.
//
// REQUIRES: ui, ui-imgui
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: ln %hermes-node-ui %t.dir/hermes-node-ui 2>/dev/null || cp %hermes-node-ui %t.dir/hermes-node-ui
// RUN: %timeout 60 %t.dir/hermes-node-ui %S/imgui-missing.js %t.dir/hermes-node-imgui.node %hermes_node_imgui | %FileCheck %s
// RUN: rm -rf %t.dir

// CHECK: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// CHECK-NEXT: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// CHECK-NEXT: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// CHECK-NEXT: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// CHECK-NEXT: {{^}}UNLOADABLE ERR_DLOPEN_FAILED path=true{{$}}
// CHECK-NEXT: {{^}}LOADED version=1.93.0{{$}}
// CHECK-NEXT: {{^}}DONE{{$}}
