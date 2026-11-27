// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// `require('hermes-node:imgui')` loads the addon beside hermes-node-ui, once.
//
// - `require.resolve()` of the name returns the specifier itself, and is
//   what loads the addon here: the embedder route initializes a module for
//   both calls (spec: Loading by name).
// - Two `require()` calls after it return the identical object, and the
//   addon's init trace appears exactly once on stderr, so neither call
//   loaded it a second time.
// - `version` is ImGui's own `IMGUI_VERSION`.
// - `KEYS` is every export, sorted. It is the guard that the test hooks
//   are absent when HERMES_NODE_UI_TEST_HOOKS is not set; later tasks
//   extend the line as they add exports.
// - Plain hermes-node does not register the name.
//
// No window, so %timeout rather than %with-display.
//
// THE MUTATION THIS MUST BREAK: trace the init line twice in
// addons/imgui/imgui_module.cpp; the ERR run fails on its -NOT line.
//
// REQUIRES: ui, ui-imgui
// RUN: env HERMES_NODE_DEBUG_NATIVE=UI %timeout 60 %hermes-node-ui %s > %t.out 2> %t.err
// RUN: %FileCheck --check-prefix=OUT %s < %t.out
// RUN: %FileCheck --check-prefix=ERR %s < %t.err
// RUN: %timeout 60 %hermes-node %s plain | %FileCheck --check-prefix=PLAIN %s

'use strict';

if (process.argv[2] === 'plain') {
  try {
    require('hermes-node:imgui');
    console.log('PLAIN loaded');
  } catch (e) {
    console.log('PLAIN ' + e.code);
  }
} else {
  console.log('RESOLVE ' + require.resolve('hermes-node:imgui'));
  const a = require('hermes-node:imgui');
  const b = require('hermes-node:imgui');
  console.log('SAME ' + (a === b));
  console.log('VERSION ' + a.version);
  console.log('KEYS ' + Object.keys(a).sort().join(','));
}

// OUT: {{^}}RESOLVE hermes-node:imgui{{$}}
// OUT-NEXT: {{^}}SAME true{{$}}
// OUT-NEXT: {{^}}VERSION 1.93.0{{$}}
// OUT-NEXT: {{^}}KEYS version{{$}}

// ERR: {{^}}[ui] imgui: addon loaded (imgui 1.93.0){{$}}
// ERR-NOT: imgui: addon loaded

// PLAIN: {{^}}PLAIN MODULE_NOT_FOUND{{$}}
