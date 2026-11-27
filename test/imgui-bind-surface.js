// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The exact export list of `hermes-node:imgui`, against the checked-in
// surface (test/fixtures/imgui/bind-groups.js).
//
// - `EXPORTS ok`: the module's own property names, minus `__test`, with
//   `drawList` replaced by `drawList.<name>` for each of its own, sorted,
//   equal bind-groups.js's all(). Otherwise `EXTRA <names>` (exported, not
//   listed) and/or `MISSING <names>` (listed, not exported).
// - `CONSTANTS ok`: every sample in bind-groups.js's `constants` equals the
//   module's value; otherwise `WRONG <name>=<got> want <value>`.
//
// No window: it only loads the module and compares, so %timeout rather
// than %with-display. The test hooks are on so that `__test` is present
// and is what gets excluded, rather than being absent by luck.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  one name removed from bind-groups.js: `EXTRA <name>`;
//  one name added to bind-groups.js that is not exported: `MISSING <name>`.
//
// REQUIRES: ui, ui-imgui
// RUN: env HERMES_NODE_UI_TEST_HOOKS=1 %timeout 60 %hermes-node-ui %s | %FileCheck %s

'use strict';

const bindGroups = require('./fixtures/imgui/bind-groups.js');
const imgui = require('hermes-node:imgui');

function exportedNames() {
  const names = [];
  for (const name of Object.getOwnPropertyNames(imgui)) {
    if (name === '__test')
      continue;
    if (name === 'drawList') {
      for (const sub of Object.getOwnPropertyNames(imgui.drawList))
        names.push('drawList.' + sub);
      continue;
    }
    names.push(name);
  }
  return names.sort();
}

const exported = exportedNames();
const listed = bindGroups.all();
const extra = exported.filter((n) => !listed.includes(n));
const missing = listed.filter((n) => !exported.includes(n));
if (extra.length === 0 && missing.length === 0) {
  console.log('EXPORTS ok');
} else {
  if (extra.length !== 0)
    console.log('EXTRA ' + extra.join(' '));
  if (missing.length !== 0)
    console.log('MISSING ' + missing.join(' '));
}

const wrong = [];
for (const [name, value] of Object.entries(bindGroups.constants)) {
  if (imgui[name] !== value)
    wrong.push(name + '=' + imgui[name] + ' want ' + value);
}
console.log(wrong.length === 0 ? 'CONSTANTS ok' : 'WRONG ' + wrong.join(' '));

// CHECK: {{^}}EXPORTS ok{{$}}
// CHECK-NEXT: {{^}}CONSTANTS ok{{$}}
