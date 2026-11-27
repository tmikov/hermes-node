// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The vendored Dear ImGui is pinned by tag, recorded in
// external/imgui/README.md. The header carries its own version
// (IMGUI_VERSION), so the README's tag must be `v<IMGUI_VERSION>-docking`,
// and the header must be the docking branch's (IMGUI_HAS_DOCK), since the
// addon turns docking on. A bump that updates one and not the other fails
// here.
//
// (Mutation: change the README tag to v1.92.0-docking.)
//
// REQUIRES: ui-enabled
// Opens no window and ends naturally.
// RUN: %hermes-node %s %source_dir | %FileCheck %s

// CHECK: README ok
// CHECK: DOCKING ok

'use strict';

// Bounded from inside the script, as test/sokol-pin.js is: unref'd, so it
// fires only if something else holds the loop open.
setTimeout(() => {
  console.log('WATCHDOG: the run did not end within 60 s');
  process.exit(124);
}, 60000).unref();

const fs = require('fs');
const sourceDir = process.argv[2];

const header = fs.readFileSync(sourceDir + '/external/imgui/imgui/imgui.h', 'utf8');
const v = /#define IMGUI_VERSION\s+"([^"]+)"/.exec(header);

const readme = fs.readFileSync(sourceDir + '/external/imgui/README.md', 'utf8');
const m = /Pinned at tag `([^`]+)`/.exec(readme);
const expected = v && 'v' + v[1] + '-docking';
console.log(
    'README ' +
    (m && expected && m[1] === expected ?
         'ok' :
         'WRONG README=' + (m && m[1]) + ' HEADER=' + expected));

console.log(
    'DOCKING ' +
    (/^#define IMGUI_HAS_DOCK\b/m.test(header) ? 'ok' : 'WRONG no IMGUI_HAS_DOCK'));
