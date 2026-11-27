// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// hermes-node-imgui.node exports its N-API registration and nothing else.
//
// ImGui is compiled into the addon, and its symbols must stay the addon's
// own: exported, they would be interposed against any other copy of ImGui
// in the process (a second addon carrying one), and whichever loaded first
// would silently serve both. The addon's CMake target sets hidden
// visibility for that reason, since Hermes's own -fvisibility=hidden stops
// at its subdirectory. This reads the defined external symbols back with
// `nm` and requires them to be exactly the two NAPI_MODULE_INIT defines.
// Anything else is printed: an ImGui name (`ImGui...`, `Im` + an upper-case
// letter) is the failure this exists for, and any other name is reported
// too.
//
// THE MUTATION THIS MUST BREAK: remove the three visibility properties from
// addons/imgui/CMakeLists.txt. Every ImGui function is then exported.
//
// REQUIRES: ui, ui-imgui
// RUN: %hermes-node %s %hermes_node_imgui | %FileCheck %s

'use strict';

const { spawnSync } = require('child_process');

const file = process.argv[2];
const args = process.platform === 'darwin' ? ['-gU', file]
                                           : ['-g', '--defined-only', file];
const nm = spawnSync('nm', args, { encoding: 'utf8' });
if (nm.status !== 0) {
  console.log('NM FAILED status=' + nm.status + ' ' + (nm.stderr || ''));
  process.exit(1);
}

// Mach-O prefixes every C symbol with an underscore.
const strip = (name) =>
  process.platform === 'darwin' && name.startsWith('_') ? name.slice(1) : name;
const allowed = new Set([
  'napi_register_module_v1',
  'node_api_module_get_api_version_v1',
]);

const names = nm.stdout.split('\n')
  .map((line) => line.trim().split(/\s+/))
  .filter((fields) => fields.length >= 2)
  .map((fields) => strip(fields[fields.length - 1]));

const imgui = /ImGui|Im[A-Z]/;
const offenders = names.filter((n) => !allowed.has(n));
const missing = [...allowed].filter((n) => !names.includes(n));

if (offenders.length === 0 && missing.length === 0) {
  console.log('EXPORTS ok');
} else {
  console.log('EXPORTS bad: ' + offenders.length + ' unexpected (' +
              offenders.filter((n) => imgui.test(n)).length + ' ImGui), ' +
              missing.length + ' missing');
  for (const n of offenders.slice(0, 20))
    console.log('UNEXPECTED ' + n);
  for (const n of missing)
    console.log('MISSING ' + n);
}

// CHECK: {{^}}EXPORTS ok{{$}}
